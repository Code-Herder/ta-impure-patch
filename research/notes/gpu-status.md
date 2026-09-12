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
| Mouse cursor position, clicks, minimap view rect, scroll rate | G13e, cured G13m, **cursor ours again G17c** | `tagpu_zoom.c`; G13m made the engine's own cursor correct by telling it the truth about the pointer, and the zoom composite stopped touching it (§2.3d). **Since G17c the cursor is DRAWN BY US** — 1× device pixels at every `k`, from the client point the message carried, into the GL UI renderer's sharp layer — and the engine's is erased from the frame by two cooperating exemptions, the UI layer's rect discard and the world composite's `uCurs` ([GL UI renderer](gui-renderer.html) §17). §2.3d's rule is unchanged and still what keeps the two in the same place |
| Which cursor sprite the engine picks on hover (move / reclaim / …) | G13j | one byte patch in `tagpu_patches.c`; the engine still draws it — see §2.6 |
| The engine's *addressable* viewport at zoom < 1 — clicks, orders and unit picking in the outer ring | G13f | `vpwide`: 3 call-site redirects + a 3-site byte patch behind `vpwide.on`, plus the `0x499221` redirect that also carries the zoom's mouse-point repair and is armed by `zoom.on` too (§2.3d) |
| Terrain in **restored true colour** (Classic++, `tagpu_classicpp.on`) | G14a (spike, 2026-09-04); GPU G14b (2026-09-04); **GLSL G14c (2026-09-05)** | `tagpu_restoreglsl.c` runs the unditherer's full model as **fragment passes in the game's own GL context** — the shaders of `tagpu_restore_glsl.h`, the weights of `<model>.w32.bin` — sliced from `tagpu_terr.c`'s gather at 12 ms of GPU time per frame under a `GL_TIME_ELAPSED` budget, visible tiles first, painting straight into the terrain pass's RGBA atlas: Two Continents' 5062 tiles in 2.1 s at 59.7 fps, the biggest stock map's 11,561 in 4.2 s, no worker thread, no runtime, no disk. The ONNX Runtime path (`tagpu_restore.c`, G14a/b) was deleted the same day (G14d). **G14e (2026-09-05)**: the cells show as they land, centre-out (the restored atlas's alpha is the flag), and the **feature and effects atlases restore lazily** — `tagpu_gaf.c` queues every atlas miss to the same restorer, each atlas carries an RGBA8 twin the sprite shaders sample where its alpha is 1, keyed texels inpainted by a nearest-ring stand-in in the FILL pass. **G14f (2026-09-05): lit** — the terrain from the engine's height grid (`main+0x14287`, one R8 texture per map, the lab's grid normal per fragment), the units from the posed face normal carried in the vertex stream, the feature sprites from the ground's lambert at their anchor; one rule, `tagpu_glsl.h` `TAGPU_GLSL_LIGHT_FN`, level ground exactly 1.0; the knobs in `tagpu_classicpp.cfg` (`tagpu_classicpp.c`). **G14g (2026-09-05): the unit textures** — `tagpu_render3do.c`'s atlas is a `TAGPU_GAFATLAS` now, every frame in a 4-texel-padded, 4-aligned cell, its RGBA8 twin restored lazily like the sprites' (priority 3) and **mipmapped to level 2**, trilinear and 4× anisotropic, the mips regenerated after each painted batch; the unit shader samples it where its alpha says so. Classic's R8 atlas has the same cells and does not move a pixel (`tascene ab`, and the engine shot byte-identical to G14f's outside the chat). Reads only — see [Classic and Classic++ renderers](renderers.html) §4c and §5 |
| **Chat, dialogs, side panel, minimap, top bar, the shell** | **Phase E, in progress** ([GL UI renderer](gui-renderer.html), decided 2026-09-06). **G15a done 2026-09-07**: every UI pixel-writing leaf is observed and the census explains 100 % of what changes on the presented surface across the screen inventory. **G15b done 2026-09-07**: the observed ops are replayed into GL twins of the engine's surfaces and the presented surface's twin is drawn over the world composite — the in-game panel, build pages, bars, option screens, chat and the F4 popup, and the whole shell, at 1:1 Classic; **0 differing pixels and 0 holes under `strict` on every in-game stop of the inventory at 1024×768 and at 1920×1080** (§2.3e). **G15c done 2026-09-07**: the rest of the in-game frame reached and measured — ARM and CORE, both resolutions, the HUD strings, the popups and the option screens over the world at 0.5× and 2×, the minimap under motion: 32 of 32 stops at 0/0/0 in all four runs, no DLL change. **G15d done 2026-09-07**: the shell across the 640×480 context switch, three game→shell→game cycles in one process, clean — the publisher drops batches while the render thread is dead or crawling (the switch stops it and it crawls out of a game), skips the stale queue after the new GL context, retires the main offscreen's dead entry, and resolves the twin through the palette the frame is *presented* with, not `main+0x143A7` (the engine gamma-scales the presented one) | the engine's surface is still drawn and still the fallback beneath the twin (nothing is suppressed in phase 1); the cursor stays the engine's; **G15e done 2026-09-08 and its Q2 diff closed 2026-09-09** (§2.3e and [GL UI renderer](gui-renderer.html) §14: the UI restores inside the Q2 bar at both resolutions, in the shell and in game, and the exclude list does not grow); the world passes reading `main+0x143A7` were wrong at Gamma ≠ 12 — **fixed 2026-09-09**, §2.3f, the world now resolving through the presented palette (`tagpu_pal.c`). How often Gamma ≠ 12 is **not** a property of the setup: the template and all 58 instance prefixes are one inode that wine rewrites at launch, now reading 12, i.e. `paldiff=0` and no seam ([GL UI renderer](gui-renderer.html) §15). **G17a's owner decision — that the world stays on `main+0x143A7`, the lab being the reference — was taken in parallel with that fix and is SUPERSEDED by the owner on this landing** ([GL UI renderer](gui-renderer.html) §15 "Not closed here"): at `Gamma` 12 the factor is 1.0 and the two tables are bit-identical, so the cost it was rejected on is zero today; the lab staying on `palette.pal` is the residual, stated there; on by default since 2026-09-08 (§2.8; `tagpu_gui.off` turns it off, `tagpu_gui.on` still carries the tokens); **phase 2 — the UI scaled — was designed 2026-09-08** ([GL UI renderer](gui-renderer.html) §13, roadmap G17a–e: the 1× mirror kept as the oracle with a device-res sharp layer beside it, a string op for text, our cursor at 1× device size, the minimap regenerated with the engine's dots) |

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
  against the engine's own posed vertices is 2e-5 model units per piece. *[2026-09-08: that
  probe folded the heading ALONE, which is right only where the bank and the pitch are zero —
  the omission Gate 0 found ([GPU posing](gpu-posing.html) §0). The replacement pass still makes
  it, so a glTF unit is posed without the terrain's tilt.]* It also caught the
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
| `0x4843C0` | screen fog-grid rebuild | `terrown` | *called by us*, not patched — and **replicated in C** by `tagpu_fogwide.c`, which builds the same masks over a window the whole zoom range fits in, **every tick since G13s** so that the first zoomed-out frame of a gesture cannot outrun it (terrain-depth §8, §8a) |

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

**Everything anchored to a unit takes the unit pass's sub-pixel anchor — including, since
2026-09-09, the health bar and the group digit — and every one of them is quantised AFTER the
zoom, not before.** Two separate defects, found and fixed the same day, and the second was
created by the first fix.

**(a) The anchor was the wrong one.** Until 2026-09-09 the bar gather read the engine's integer
world shorts directly, which pinned the bar to the SIM rate while the body glided at present
rate; the two slid against each other by up to a whole sim step of motion. The old error was
**proportional to how far the unit moves per sim step**, so it grew with unit speed and with
`gamespeed` — 1.68 px peak-to-peak at 1x at TA's normal speed, 2.95 at `gamespeed` 20, and
`zoom` times either on screen.

The rule now is **the bar sits on whoever drew the body**, and it takes two branches because
two different things draw units. When `tagpu_native_owns_unit()` holds — the very predicate the
unit pass gathers on — the body came from `tagpu_native_unit_pos()`, so the bar takes that same
number. When it does not, the unit pass skipped the unit and the **engine** drew it from `(s16)`
reads of the same 16.16, and the bar floors with it. That second branch is not hypothetical:
`markown` suppresses the engine's own bars globally, so a unit the type filter rejects, or a
nanoframe while the build-effect detour is absent, still needs a bar from us.

*[The landing review caught this. Both branches went through the accessor at first, on the
belief that it reports "no sub-pixel sample" — it does not. `tagpu_native_unit_pos` returns 1
whenever its **pointer** checks pass and hands back the raw fraction when the table holds no
sample, so the integer branch was unreachable and every engine-drawn unit got a bar up to a
whole game pixel off its body. The code comment and this note both asserted the opposite, that
the sampleless path was byte-for-byte what it had always been.]*

**(b) Then it was quantised on the wrong grid.** The first fix floored that anchor, on the
argument that the selection rect floors the same anchor and the two should agree. They did
agree — with each other, in the frame's PRE-zoom units, which is the wrong grid. The vertex
shader scales this pass by `zoom` about the zoom centre, so **one unit of quantisation here is
`zoom` displayed pixels**: invisible at 1x, 4 px at 4x, 8 px at `ZOOM_MAX`. The bar stood still
and then teleported 4 px while the body glided underneath it, which is what the owner reported
as a diagonal twitch at max zoom-in. (The selection rect never showed it because it does not
actually floor in these units — `tagpu_native.c` snaps its corners *forward through the zoom*,
floors there and comes back. The glyph atlas has done the same since G15.) `tagpu_mark.c`'s
`snap_device()` is now that rule, shared by the bar, the group digit and the text, so the step
is **1/ss of a displayed pixel at every zoom** and the bound does not grow with the zoom at all.

**Measured** on a walking commander, 1920x1080, camera pinned, TA's normal game speed, at 4x —
the separation between the bar and the body it sits over, in *displayed* pixels
(`tagpu_spxlog.on`; the model is exact and build-independent, which is what makes one run report
all three rules):

| the bar's anchoring rule | rms | peak-to-peak |
|---|---|---|
| the engine's shorts (the original defect) | 1.35 px | **6.56 px** |
| the body's anchor, floored (fix 1, superseded) | 1.15 px | **4.00 px** — exactly `zoom` |
| the body's anchor, `snap_device` (ships) | 0.14 px | **0.49 px** — 1/ss, at any zoom |

Corroborated from video on the two builds themselves, same instrument each leg
(`tools/barwobble_detect.py`, `scenarios/bar-wobble-4x.json`). The statistic that catches this
is **the bar against itself**: a unit walks at constant speed, so a bar that tracks it has a
second difference near zero and a bar quantised on a grid of `q` px stands still and then
teleports `q`. Before: still on **60.9 %** of presented frames, and every step it did take was
exactly 0, 4.00 or 8.00 px with nothing in between; |2nd diff| 2.09 px mean, **4.00 px p99**.
After: still on **2.3 %**, a continuous 1.1–3.2 px spread tracking the unit's real speed;
|2nd diff| 0.42 px mean, **1.00 px p99**. At 1x both builds pass every 1x oracle equally —
that is the point of (b), and the reason the 4x fixture is now a tracked one.

**Stock TA cannot show either artifact**, because there the bar and the body are the same shorts.

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

**And since 2026-09-11 it can be drawn as GEOMETRY instead, behind
`tagpu_selgeom.on`** — each edge two triangles, a band of `w` game pixels
(`w=`, or `wdev=` in device pixels) expanded along the edge's *minor* axis,
because Bresenham's own rule is one pixel per major-axis step. That is the width
the driver would not give us, and it is the thing `devres` was waiting on: at
`k = 1.5` the rect reaches full colour (1019 device pixels at ≥ 0.9 coverage,
peak 1.000) where the GL line reaches 5 and peaks at 0.928. It is **bit-identical
at 1:1** — 0 differing pixels and an unmoved md5 at `ss = 2` with the resolve, at
`ss = 1`, at zoom 0.5 and 2.0, and on the slope fixture — and off by default.
`selgeom main` turns `selAt1x` off for the A/B; that costs 1320 pixels at
`ss = 2`, so the 1x detour is still what makes the default exact.
[UI markers](ui-markers.html) §1 has the numbers and the two construction traps
(the cap must run along the segment; the band is nudged 1/256 px off the tie).

Two consequences worth knowing. **Supersampled — the default — the rect draws after the marker
layer**, so where a box edge crosses a health bar our line wins where the engine's bar would (the
bars sit inside the box on every stock unit measured); the fallback site under `tagpu_ss.off`, or
without `glBlitFramebuffer`, still draws it first, under them, so the two sites layer differently. And the same supersampling that dimmed the rect dims **every line
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

### 2.3a-bis The arm state is published in one store, never transiently zero  [2026-09-10]

The arm block inside **`tagpu_native_frame()`** (there is no `tagpu_native_armed()`; the other
passes have one, this one does not) re-reads its lever every 30 frames. It used to do that by setting
`s_armed = 0`, performing a **file read**, and setting it back — and `tagpu_native_owns_unit()`
opens `if (s_armed != 1) return 0;` and is called **from the game thread** by `tagpu_markown.c`'s
`mark_selbox`. So twice a second, for the length of that read, every selected unit read as "not
ours" and the engine drew its own selection rects into the key-filled viewport, where the GUI
mirror published the boxes and painted them cyan over the world (`gui-renderer.md` §20).

It is now computed into locals and published in **one store**, and `s_armed` is **`volatile`**
(as `s_selComplete` already was) so that store cannot be hoisted above the state it publishes.
That closes the periodic window — the one the shipped configuration hits twice a second.

**Residual, stated rather than papered over:** `s_type` is written only when it has actually
changed, and the pass is disarmed across that write, which **narrows** a window rather than
removing one — nothing waits for the game thread to observe the disarm. It is reachable only
while a human is editing the arm file. Closing it properly wants the type published by index into
a double buffer, which is its own piece of work.

**And one predicate carries the whole ownership decision [2026-09-10].**
`tagpu_native_owns_unit` now also refuses a unit whose **ModelId will not resolve**, because that
decision has four consumers that must agree: the gather skips what it refuses, `tagpu_overlay.c`
leaves the engine's composite unwiped, `tagpu_mark.c` leaves the bar on the engine's anchor, and
`tagpu_markown.c` leaves the engine's own selection rect alone. Wrong in one direction a unit is
drawn twice; wrong in the other it is invisible, or keeps its sprite and silently loses its
selection box for ever — which is what a landing review caught here, against a claim that a unit
with no model was "owed nothing" (`ui-markers.md` §1 carries the correction and the disassembly).

**The general rule this is an instance of:** anything the game thread reads to decide whether we
own a draw must never have a "not yet" state that the render thread publishes on its way to an
answer. A poll that tears is a poll that hands the engine back the frame.

### 2.3e Zoom to the cursor (`tagpu_zoom.c`, no arm file)

**[CORRECTED 2026-09-12, frame packet landing 2 — §2.17 is the current mechanism.]** The rule,
the arithmetic and the three properties below are unchanged and were re-measured on the landing
(the same four gestures, exact on every axis; the hover oracle; the follow release). What
changed is WHERE the eye is written: the render thread no longer touches `main+0x1431F` or any
other engine word. It works out the step in `tagpu_zoom_read_lever()`, adds it to a cumulative
sum that rides the command record, and draws this frame from the packet's eye plus every delta
the game thread has not acknowledged; the game thread applies the difference at the top of its
next in-play draw, releases the follow there when the gesture asked, clamps, recomputes the
minimap box and clears the fog flag itself. So the paragraphs below about *render-thread*
stores, the `s_dropFollow` request that `terr_fogtick` consumed, the residual "kept whole while
it waits", the fog request/ack pair and the gate on terrown owning the fog draw describe the
2026-09-10 mechanism and are kept as its record. The "named gaps" paragraph's read-modify-write
on an unaligned field racing the stepper is closed: one thread writes the eye now.

The wheel holds the world point under the **pointer** still, instead of the one at the centre
of the screen. No engine patch and no lever: the transform is a similarity about the viewport
centre and nothing in it is free, so the one variable that can hold a point is the engine's
eye. With `W(s) = eye + vw/2 + (s − c)/z`, holding `W(a)` across a change in `z` is

    d = (a − c) · (1/z_prev − 1/z_now)

added to the eye, where `a` is the point the notch was aimed at. Applied on the render thread
inside `tagpu_zoom_read_lever()`, which is the call the pass makes before it reads the eye, so
the zoom and the eye it is drawn with change together.

**Fields we write:** `main+0x1431F`/`+0x14323` (the eye) and `main+0x14327`/`+0x1432B` (its
scroll target) — always together, because the two disagreeing is what the per-frame stepper
reads as a camera move in flight and would rebuild the fog grid every frame for as long as it
lasted (G13g) — `main+0x142CB`, the minimap's view box, recomputed through the engine's own
`0x466B70` because `0x41C3C0` is the only place it is otherwise filled, and, on a frame that
actually steps the eye, the three **camera-follow** slots zeroed (below). **No engine flag bit is
written at all**, and both omissions are deliberate: see the two new rows in
[exe reverse engineering](exe-reverse-engineering.html) on `main+0x14281` bit 3 and
`main+0x142F1` bit 1.

**A frame that wants to step the eye asks the GAME THREAD to release the camera follow**
[G13u, 2026-09-10]. Following a unit, the stepper `0x41CA10` recomputes the scroll target from it
*every* frame and clamps it inline, so the anchor's delta was eased straight back out and the zoom
read as pinned to the unit. The engine answers the same problem in its own edge and hotkey scroll
by releasing the follow — the scroll poll's eye-writing tail clears `main+0x1434B`, `+0x142F3` and
`+0x142F7` at `0x41D091`…`0x41D0AA`, the body of `0x41C390`, and `0x41D035` skips that tail on a
frame where the eye did not change.

**Those three stores are ours to make only on the game thread, and that is the whole safety
argument** — the same handshake as the fog request above, for the same reason plus one more.
`main` is **randomly misaligned at every launch** (`0x41D920` pads it by
`(GetTickCount() % 1000) * 7`; see [exe reverse engineering](exe-reverse-engineering.html)), so
`main+0x142F7` is 4-aligned in only 25 % of launches, and a dword there is tear-capable in 4.7 % of
them on Intel's documented guarantee (it straddles a cache line) and in **37.5 %** on AMD's (it
straddles an aligned 8-byte boundary; the reference setup is an AMD part — [cross-thread engine
reads](cross-thread-engine-reads.md) §3). A cross-thread store of zero could therefore be torn — into
a slot the stepper **dereferences** at `0x41CA58`/`0x41CA95`. The eye and the scroll target are misaligned
in exactly the same way and are written from the render thread anyway, because a torn *coordinate*
is bounded by `clamp_pair()`; a torn *pointer* is a wild read.

So: `anchor_step()` publishes a level — `s_dropFollow`, "a gesture is trying to move the camera
right now" — and `tagpu_zoom_follow_tick()` does the stores from `terr_fogtick`, gated on the same
`tagpu_terrown_owns_fog()` that anchoring itself is gated on, so the request always has a consumer.
Three consequences, all deliberate:

* **The residual is kept WHOLE while it waits.** A frame that finds a follow set returns without
  spending `s_residX`/`s_residY`, so the gesture's total displacement is still exactly
  `(a − c)(1/z_start − 1/z_end)` across the wait rather than losing the frames it spanned.
* **A level, not an edge**, because the engine's own writers are guard-then-store with the compare
  well before the store — 43–46 bytes for the countdown writers, 8 for the two that write the
  object slot (`0x49AE84`→`0x49AE8C`, `0x49C7F3`→`0x49C7FB`): a follow re-established in that
  window would survive a one-shot request, and is taken away again on the next tick.
* **The game thread CONSUMES the request and the render thread re-arms it** every frame it still
  wants the camera. That bounds the one thing a level cannot bound by itself, a producer that
  *stops*: `terrown` keeps skipping for up to 90 frames after the native pass goes quiet, and a
  frozen level would spend those frames deleting every follow the player established. Consumed, a
  dead producer costs exactly one release.
* **A game thread that stops DRAWING never releases, and the anchor then never steps the eye** —
  the camera keeps following, which is the old behaviour and the fail-safe direction. **Pausing the
  sim is not that**: `0x4848E0`'s sole call site `0x469D8E` is inside the per-frame world draw, so
  a paused game still services the request.
* **The banked debt dies with the claim.** A gesture that ends without ever getting the camera
  voids its residual (`drop_claim()`), and so does every exit that gives the claim up. Without that
  the bank survives the gesture — nothing else spends it, since every later frame returns at
  `zNow == s_zStep` — and the next notch anywhere on the map discharges it in one frame as a silent
  camera jump. It is also what kept the centre-aimed control honest: `nx` tests the *accumulated*
  residual, not this frame's contribution, so a banked value would make a centred wheel step and
  release. [All four found by the landing review, 2026-09-10.]

**Also in the fields we write, by consequence:** releasing the follow zeroes `CameraToUnit`, which
is `+0` of the camera block `0x469BFC` hands the order-marker driver — so from that frame an
**unselected** followed unit's order lines stop being drawn (`tagpu_order.c` reads it as `camU`; a
*selected* unit takes the same mask on the next branch and is unaffected, so the Ctrl+C case loses
its lines only while nothing is selected). Same thread, no tear, and it is the state the engine
reaches after its own edge scroll; noted because marker-parity captures depend on it. It applies
under the partial arm set too (`vpwide.on` + `terrown.on` without `zoom.on`), which is a
configuration the module otherwise declines to drive.

**Measured 2026-09-10** on the landing binary, same skirmish, commander followed with Ctrl+C, four
notches aimed at `(900, 600)` against a viewport centre of `(576, 384)` — same gesture and
direction on two builds differing only in whether the request is published, from a start eye within
2 px (the followed commander drifts between runs):

| build | gesture | eye | follow after |
| --- | --- | --- | --- |
| request removed | −4, z 1.0 → 0.683, from `(5127, 7609)` | `(5125, 7609)`, Δ `(−2, 0)` — cancelled | still set |
| as landed | −4, z 1.0 → 0.683, from `(5127, 7611)` | `(4977, 7511)`, Δ `(−150, −100)` | released |
| as landed | +4, z 0.683 → 1.0, from `(5127, 7609)` | `(5277, 7709)`, Δ `(+150, +100)` | released |

Both landed rows are the predicted `(a − c)(1/z_prev − 1/z_now)` = `(±150.4, ±100.3)` to the pixel,
so the game-thread wait costs no accuracy. The control — the same gesture aimed at the viewport
centre — moves the eye by nothing, leaves the follow **intact** and logs no release.

**The leak regression** [2026-09-10, after the review]: three gestures in a row, each re-following
with Ctrl+C first, gave Δ `(−150, −100)`, `(−150, −100)`, `(−151, −100)` against a true
`(−150.37, −100.25)` each. Totals: **X 451 against 451.1, Y 300 against 300.7**. Both sit inside
the ±1 world px that a carry entering and leaving the run can move a three-gesture total by, so
this is the carry working, not a bank — a leaked bank would have shown as a jump of hundreds, not
a pixel. *(Quoting X alone as a telescoping proof would be cherry-picking: Y is the looser of the
two and is the one to check.)* A centre-aimed wheel taken immediately afterwards, with that carry
standing, left the eye at `(5113, 7624)` unchanged and the follow set.

**Three properties the tests lean on.** The delta is exactly 0 with the pointer at the viewport
centre, so that case is the previous behaviour — which is the A/B control, and why this needs no
lever. *Precisely*, since G13u ties the follow release to the eye actually stepping: `nx` tests the
ACCUMULATED residual, not this frame's contribution, so the control holds while the carry standing
from an earlier gesture is under half a world pixel — which `drop_claim()` now guarantees, because
the only residual that survives a gesture is the sub-pixel remainder of a frame that did move the
eye. The one case left is a carry of exactly ±0.5, where `iround`'s half-away-from-zero gives ±1:
a centred wheel would then step one world pixel and take the follow with it. Not reproduced, and
not special-cased — changing the rounding would break the in-then-out oracle's exactness. The steps **telescope**, so total displacement is
`(a − c)(1/z_start − 1/z_end)` whatever the frame timing, giving an exact oracle on the eye.
And in-then-out with a still pointer returns the eye exactly.

**Four gates, each a positive condition.** The wheel only — `tagpu_zoom.txt` keeps the
centre-anchored behaviour, so every zoom fixture stays reproducible. `tagpu_eye.txt` wins, and
says so once a second. A zoomed world must be on screen. And `tagpu_terrown` must own the fog
draw, which is the safety argument rather than tidiness: only there is the grid's rebuild
decision ours to make on the game thread.

**MEASURED 2026-09-10**, 1920×1080, `crowd-static` on Two Continents, viewport `128,32
1792×1016` so `c = (1024,540)`:

| case | anchor | notches | predicted Δeye | measured |
|---|---|---|---|---|
| control | (1024,540) | +6 | (0, 0) | **(0, 0)** |
| upper-left | (400,300) | +6 | (−272, −105) | **(−272, −105)** |
| lower-right | (1700,900) | +6 | (294, 157) | **(294, 157)** |
| zoom out | (400,300) | −6 | (481, 185) | **(481, 185)** |
| at the map corner, eye already (0,0) | (140,44) | +8 | (−472, −265) | **(−472, −265)** — the eye going negative, i.e. §2.3c's widened range composing with the anchor |

Exact on every axis. In-then-out returned `(1728,702) → (1456,597) → (1728,702)`. At a map
edge the range refuses what it must and the residual is dropped rather than banked: parked at
`(0,0)`, six notches out anchored at the bottom-right held eye **and** target at `(0,0)` for
four seconds with no churn.

**The functional oracle is the engine's own hover state**, `main+0x2CBA` (the unit under the
pointer). Pointer parked on an ARMPW drawn at `(664,186)`, +6 notches: **still unit 17 under
the cursor** anchored, and unit **68** under it centre-anchored. That is the feature, read out
of the engine rather than off a screenshot.

**Fog:** `bare=0` on every `fogwide` heartbeat of the run and the replication oracle
`differ=0` (1972 of 1972 cells). A frame-by-frame scan of a lossless 60 fps capture at the
units, on G13s's green-dominance criterion, put the worst single-frame excursion of the
anchored run at **33 396 px against the eye-fixed control's 45 431** — the control cannot have
an eye-induced artifact at all, so the excursions are the ease changing how much world is on
screen, and anchoring adds nothing above that floor. The criterion was checked live before
being trusted: 196 210 green-dominant px at the units against **0** in the fogged corner.

**What the landing review found, and what it settled** (two Opus reviewers, `high`,
2026-09-10). Three real defects, all fixed on the branch: the eye was stepped *before*
`zoom_eye_range()` was consulted, so a frame with no sane engine state left it unclamped **and**
skipped the invalidation; `anchor_step()` used the widened camera range without the
`g_eyeInstalled` guard the public accessor applies, so `vpwide.on` + `terrown.on` without
`zoom.on` — a reachable arm set, since `tagpu_opt.c`'s `needs` steers a default and not a
requirement — could push the eye past a range nothing would walk it home from; and
`tagpu_zoom_fog_pending()` could latch, because `tagpu_terr.c` drops the skip later in the same
frame than `read_lever()` runs, leaving a bump nobody would ever ack. It now asks
`tagpu_terrown_owns_fog()` first: no consumer, no request.

Two claims the review disproved, corrected above and in
[exe reverse engineering](exe-reverse-engineering.html): the lowest level a single ease step can
cross 1.0 from is **0.5 exactly**, not 0.489, and the engine grid's slack collapses to the bare
32 px at 1×, not to zero. `tagpu_fogwide.c`'s coverage derivation was true but its proof was
not — it holds only under the one-ease-step bound, which the comment now states.

**And one it raised that measurement refutes.** We do not set the minimap's dirty bit, and at
`k = 1` the sharp minimap layer returns early (`tagpu_gui_surf.c:1451`), so the engine draws the
view box gated on that bit — the box should lag. It does not: measured 2026-09-10 across an
anchored gesture, **51 px of the 106×126 minimap change**, bounding box screen x 26..42 y 7..16,
exactly the union of the old box (26,7,42,16) and the new one (32,11,42,16) — erased at the old
position, drawn at the new. Something else sets the bit during a gesture; which path, we did not
establish, so this is a measured behaviour and not an explained one.

**Named gaps.** The eye is an integer in world px, so the anchor can sit up to `z/2` screen px
from the pointer while a gesture is in flight — 0.5 px at 1×, 4 px at 8×; holding it exactly
would need an off-centre scale centre, which vpwide's addressable rect, fogwide's window and
the ring test all assume away. Two sites that clamp the scroll target inline against
`[0, map − W]` — `0x41C4C0` and `0x41C7F7` — do not go through our clamp, so on those paths a
target we stepped can be recomputed without the delta and the stepper eases the eye back;
neither is a standing state, so the zoom composes with the next camera move. *[CORRECTED
2026-09-10: this listed the per-frame camera FOLLOW `0x41CAF7` as a third such site and
concluded "the camera owns itself while it is following something". It does not — a follow is
recomputed **every** frame, so it did not merely stop the delta `d` short of a map edge, it
cancelled the whole gesture's camera move (measured: the eye moved (0, −2) where the anchor
asked for (−150, −100)). `release_follow()` releases the follow instead, which is the engine's
own rule for a manual camera move.]*
`eye[0] += nx` is a read-modify-write on an unaligned field (`main+0x1431F` is an odd offset)
racing the game thread's own stepper, and the landing accepts it: every value that escapes goes
through `clamp_pair()` so nothing can address memory the engine does not own, but a lost update
**discards** a camera move rather than delaying it — a minimap click landing inside the window is
simply gone — and this change raises the write rate from "only when a clamp fires" to every frame
of every gesture. A camera hold that is *moving* (a scripted pan through `tagpu_eye.txt`) now
bumps the fog sequence every frame, so such a pan draws on the wide grid throughout even at 1×;
the replication oracle reads `differ=0`, so the picture is the same, but the fog texture is
re-uploaded on each switch. And `s_eyeHold` is polled every 15 frames, so the hold gate is up to
a quarter-second late in both directions.

### 2.3b The addressable viewport at zoom < 1 (`tagpu_vpwide.c`, `vpwide.on`)

**On by default since 2026-09-08 through the play defaults (§2.8); opt-in before that.** Nothing
here writes a byte unless the pass was on at DLL attach — `tagpu_vpwide.on`, or the default with
the zoom on and no `tagpu_vpwide.off` — the true rect verified, *and* a zoomed-out view is live.

| VA | What it is | Mechanism |
|---|---|---|
| `0x468D85` / `0x46964F` / `0x469F95` | `call 0x4C6B10` — the three sites in `DrawGameScreen` that copy the viewport rect into the offscreen surface's clip rect (`+0x1C..+0x28`) | call-site redirect; **clamped to the surface AND to the TRUE viewport rect** (2026-09-09, below), because `0x4C6B10` is a bare four-dword store with no clamping: the surface clamp keeps a wide rect from licensing a write outside the allocation, the viewport clamp keeps one from licensing a write outside the rect our key fill erases |
| `0x499221` | `call 0x498DA0` — the mouse → world / map cell / hovered feature conversion, and the ONE reader that uses L and T as the screen→world **origin** (`world = eye + clamp(pos, L, R) − L`) | call-site redirect; redone with the TRUE origin and the WIDE clamp, a straight pass-through whenever the rect is not ours |
| `0x4B5E5F` / `0x4B5EC0` / `0x4B5F0C` | the three arms of TA's own window procedure's `0x200..0x206` jump table, each unpacking the mouse `lParam` with `AND 0xffff` + `SHR 0x10` | **byte patch** → `MOVSX ECX,CX` + `SAR EAX,0x10`. `LOWORD`/`HIWORD` is zero-extending, so a client x of −20 arrived as 65516 and the event was lost; this is Microsoft's own `GET_X_LPARAM`, and for any position a real mouse can report it is bit-for-bit identical |

**The clip guard needs BOTH bounds, and the second one is the picture's [MEASURED 2026-09-09].**
Clamping the widened rect to the surface allocation is a memory-safety bound and nothing more: it
still leaves the side panel and the top and bottom strips inside the clip, and **nothing ever
repaints those**. Our key fill covers the true viewport only — that is why every pass takes its
rect from `tagpu_vpwide_true_rect()` and not from the field — and the engine repaints the panel
only on damage it knows about, which a stray world draw is not. So one frame in which an engine
drawer runs with the wide rect leaves marks outside the viewport **for the rest of the session**,
and they accumulate.

That is not hypothetical: it is the bug a 500 v 500 fight with the whole army selected produces at
zoom < 1 (`scenarios/500v500.json`, the fixture written for it). The drawer that reaches them is
the engine's own selection rect `0x46A530`, which `markown` hands the WHOLE set back to whenever
the native pass came up one box short (`ui-markers.md` §1) — and `0x467A50` projects each one at
the **unzoomed** position, which at 0.42× is up to 1.4 screens from where the unit is drawn. Two
independent runs of the fixture: **5768 stray green pixels on the panel before, 31 after** (the 31
is the minimap's own view rect), with five hand-back frames in the "after" run and the engine
surface under the buttons reading 100 % palette index 0.

**The clamp is NOT gated on the surface probe [2026-09-10].** It only ever narrows, so it needs
to know nothing about the allocation — and riding it on `IsBadReadPtr` succeeding would make the
bound conditional on a probe, which is precisely what CLAUDE.md refuses as a safety argument. A
landing review caught it there: one call with an unreadable `self` would have re-licensed the
permanent side-panel marks. The allocation clamp above it still rides the probe, because that one
genuinely needs the surface's width and height.

**Two residuals, named:** `mark.on=nocursor`'s capture window (window B) takes
`tagpu_vpwide_addressable()` and then intersects with the ctx clip, so the narrower clip costs
that **debug lever** the ring markers it exists to show — the shipped configuration never opens
it (G13p). And `0x46A530` indexes `MODEL_PTRS[ModelId]` with no zero test while slot 0 is never
written by the load loop, so an engine box drawn for a ModelId-0 unit bounds itself on
uninitialised memory. That is stock TA's own behaviour, not something this stack introduced, and
restoring it is the correct half of the `owns_unit` fix — but it is worth knowing it is there.

Clamping to the true rect is exactly the bound **stock** TA sets at these three sites — unwidened,
the rect they are handed IS the true one — so it can never clip anything the engine would otherwise
have drawn on screen, and at zoom ≥ 1 or with the widening disarmed it is the identity.

The rect it writes is `main+0x37E27..0x37E33` (L, T, R, B) only — **never W/H at `+0x37E37`/`+0x37E3B`**,
because the eye clamp `0x41C3C0` derives `maxEye = map − W` from them and a negative `maxEye`
makes it alternate between 0 and a negative eye. The true rect is derived from the **screen
dimensions** at `+0x37E1F`/`+0x37E23` — fields nothing here writes — because `0x497F40` computes
`W = R − L + 1` by *re-reading* L, so a store of ours landing in that window would corrupt W.
Until 2026-09-12 that store came from the render thread, W/H were checked against the derivation
every frame and put back when they disagreed. **Since the frame packet's landing 2 the store is
the game thread's** (`tagpu_vpwide_apply`, at the top of the in-play draw, from the level the
command record carries), and `0x497F40` runs on that same thread at game entry, before any
in-play draw — the two cannot interleave, so the repair is gone and a disagreement is only
COUNTED (`vpwh=` on the packet heartbeat, **0** over the landing's runs: 305 779 in-play draws
across two levels, 1 571 648 under the stress lever). The addressable rect the message thread's
ring test reads is published right after the field is written, so it is exactly what the engine
can name.

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
is where the camera is *heading*, and the per-frame stepper `0x41CA10` eases the eye toward it.
Paths that set the eye and copy it into the target afterwards (`0x41C574`, the path whose
clamp call is `0x41CDE1`, the scroll `0x41D037`) reach the widened range through the detour. Three sites instead compute the
target and clamp it **inline** against `[0, map − W]`, never calling `0x41C3C0` for it —
`0x41C4C0` (smooth `SetCamera`), `0x41C7F7` (smooth centre-on) and `0x41CAF7` (the per-frame
camera **follow**, which recomputes the target from the tracked unit every frame). The stepper
walks the eye to that target and our wider clamp leaves it there, so **those paths still stop
`d` short of a map edge**. (The follow is still one of the three *for the range*; what changed
is that the cursor anchor now releases it rather than competing with it — §2.3e.) Nothing fights and nothing churns — the eye arrives at a target
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
map edge would show the void past it. The command apply (`tagpu_zoom_apply`, **on the game
thread since 2026-09-12**; `apply_eye_range()` did it from the render thread before) re-applies
the same clamp at the top of every in-play draw while a zoomed world is live and writes only
when the eye is actually outside the range in force (same standing as the `ScrollSpeed` write:
local camera state no other machine sees).
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

### 2.3e The GL UI layer — observers, publisher, twins (`tagpu_gui_hook.c`, `tagpu_gui_surf.c`, `gui.on`, Phase E G15a + G15b + G15d + G15e + G17a–e)

**Phase 2 is complete.** G17e (2026-09-09) made the **minimap** ours at `k > 1`: the base from the
TNT's own 252-px picture instead of the 126-px box the engine fits it into, and the fog, the unit
dots, the radar arcs and `DrawPoint`'s points all taken from the engine's own pixels by masking
`+0x142DB` and `+0x142DF` against `+0x142E3` — so the visibility decision never leaves the engine.
2 084 distinct colours against the engine's 532 at `k = 1.5`, and 2 differing pixels of 13 356 on
a 99.6 % fogged map. G17d (2026-09-09) made text a **string op**: `PK_STRING` carries the string,
the font and `0x4CCF60`'s three colour bytes, and the render thread stamps TA's own glyphs into the
twin from a **per-font glyph cache** — the string-keyed atlas the marker path uses is wrong for a
UI whose text is a clock and a metal readout. 44 % less arena traffic where text is redrawn, and
two 120-stop walks with `miss=0`. G17a (2026-09-09) added the seam: a 4-tap sharp-bilinear ramp on the 1x mirror
run after the palette lookup, a device-resolution RGBA8 **sharp layer** above it, and `k` read off
the frame. G17b (2026-09-09) made `k != 1` live and added device-space click injection. **G17c
(2026-09-09) put the cursor in the sharp layer** — ours at 1x device pixels at every `k`, from the
client point rather than the engine's logical grid, with the engine's own erased by the UI layer
and the world composite together ([GL UI renderer](gui-renderer.html) §15–§17). Still no engine
patch in phase 2 and no new engine-state write.

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
| `0x4D85A0` | `MEM_Free(block)` — the allocator's own free, **not a pixel writer** | 5 | retires the surface whose block it is (G18-8): `block+0x30` is the pixel base, and this is the only way an engine allocation dies |
| `0x4A81E0` | `GUI_StageUpdateDraw(gi, flags)` | 10 | a build/redraw event for the log |

**Two writers of the minimap composite are missing from this table, and what saves them is an
accident** [VERIFIED 2026-09-08]: the radar coverage arcs `0x4C0070` and `DrawPoint 0x4BEE60`
write `main+0x142DB` and nothing observes them. They land in the twin regardless, because
`0x466DC0` copies the base in first and that copy's source `main+0x142DF` — written only by
`0x466C20`'s direct byte writes — is never seeded, so the copy publishes as a **pixel op carrying
the destination's final bytes**, arcs included. **Seed copy sources on demand and the arcs
disappear**: the copy would become a real twin→twin `PK_COPY`. The census cannot catch it either
way, since those pixels sit inside the copy's box ([GL UI renderer](gui-renderer.html) §7).

Tokens in `tagpu_gui.on`: `strict` (the fallback off, a miss painted magenta, the cursor rect
exempt — the harness's mode), `off` (the detours stay installed for the next launch, nothing is
published or drawn — the live A/B lever), `norestore` (G15e: the layer without Classic++ art),
`sharptest` (G17a: the sharp layer filled with a known pattern — the harness's mode too),
`nocursor` (G17c: phase 1's cursor, the engine's own — the A/B against ours), `cursorscale=N`
(G17c: our cursor's size in device pixels per art pixel, default 1, clamped 0.25–8),
`nostring` (G17d: text stays a box of captured pixels — the A/B, and **read at ATTACH like
`census`/`log`/`pgm`/`trace`, so it must be armed before the launch**; only the tokens the *surf*
module owns follow the file live), `nominimap` (G17e: the engine's minimap back) and `mmbase`
(G17e: ours forced on at `k = 1` too, where it is otherwise off — the harness's A/B),
`census` (the G15a diff), `log` (a census line per 50
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
  fallback wherever a twin has nothing. The index is resolved through **the palette the frame
  is presented with**, not `main+0x143A7` (G15d, below). The cursor's rect
  (`*(0x51FBD0)+0x1B2/+0x1B6/+0x1BA`) is left to the engine's frame. `strict` paints a miss
  magenta instead of falling back, outside the viewport or on a non-key pixel inside it.
- *The seam, since G17a (2026-09-09):* that one draw now composites **three** layers, top down —
  the **sharp layer** (one `RGBA8` texture at the *device* resolution, row 0 the viewport's top,
  cleared every present, taken where its alpha says it has coverage), the **1× mirror scaled by a
  sharp-bilinear ramp**, then the engine's frame. The ramp is a 4-tap run **after** the palette
  lookup — interpolating indices is meaningless — with each tap premultiplied by its own coverage
  so an uncovered texel contributes nothing rather than dragging index 0 in from the key fill, and
  the coverage thresholded at 0.5 after the blend. Its width is one *device* pixel, so at `k = 1`
  it is one source texel and the blend collapses to the single tap `texelFetch` would have taken —
  identical after 8-bit quantisation, with three decades of margin, which is G17a's whole gate.
  `k` is `vp_w / twin_w` read off the frame; the *engine* running at `window / k` is G17b's, but
  **`k` is not 1 on every phase-1 path**: `resizable` defaults TRUE and `maintas` fits the
  viewport to the client, so any window dragged off the game resolution is already fractional.
  At *integer* `k` the ramp is exactly nearest (measured at `k = 2`). The sharp
  layer is **empty** until the cursor (G17c) and the string op (G17d) fill it, so the token
  `sharptest` fills it with a known pattern — that is how an empty layer is testable at all.
  Measured: the parity fixture's frames byte-identical to `main`'s with Classic++ off and on, the
  120-stop `strict` walk unmoved, `fps=60.0` ([GL UI renderer](gui-renderer.html) §15).
- *Classic++ (G15e, 2026-09-08):* every surface may carry a **colour twin** — `GL_RGBA8`, the
  same size, `COLOR_ATTACHMENT1` of the twin's own FBO — and the sprite and copy programs are
  **MRT**, so one draw writes the index and the restored colour together. A sprite takes the UI
  atlas's restored texel where the atlas has one (alpha 1); **a copy carries both channels**,
  which is what makes restored art survive the `panel+0xBC` → frame blit, and a copy from a
  source with no colour twin writes zero and so invalidates the destination there. Seeds and
  pixel ops carry indices only and drop the colour of their box. The layer chooses **per texel**:
  restored where alpha is 1, the live palette elsewhere. **The palette-validity rule**: the
  restore job snapshots the palette into a texture of its own, so its colours hold only while
  that is still the palette the frame is *presented* with — compared every frame, colour ignored
  while they differ, and after 30 still frames the job is rebuilt against the new palette and
  every colour twin invalidated. **The UI also steps the restorer when nothing else does**: `tagpu_rglsl_step`'s
  only other caller is the native pass, which returns early with no unit array — in the shell and
  with the world passes disarmed — so without this the UI atlas's queue is never drained and the
  shell stays indexed for ever (found by the landing review; `tagpu_rglsl_calls()` compared across
  presents means the budget is still sliced once per frame). The atlas restores at **priority 4**
  (`MAX_JOBS` is 6 since this landing; 0–3 are terrain, features, effects, 3DO units) and skips
  frames under **12×12**
  (`restoreMinEdge`, G15-0's verdict). Trigger token **`norestore`** is the A/B. Heartbeat gains
  `cpp= col=<made>/<live> colvalid= rearms= rgb=`. **Colour reaches a twin only through a sprite
  op, so seeded art stays indexed until the engine redraws it** — entering a game, the panel is
  seeded and indexed until a repaint. Measured: `fps=60.0`, 37 415 of 45 056 px of the menu's
  panel rect against `norestore`, **7 963 px of the 640×480 shell frame**, and `+gamma 15` → 235
  entries differ, one re-arm, colour valid again ([GL UI renderer](gui-renderer.html) §14).
  **The gate closed 2026-09-09 with the Q2 diff** (`uiwalk.py --restore` fills the atlas,
  `tascene uidiff` holds the dumped twin to the same restorer run offline on the dump's own
  cells): shell and game, 1024×768 and 1920×1080, **far band max 1 level on 0.0007–0.0017 % of
  bytes, 0 unmatched, alpha and the replicated border exact**, and a near band bounded *tighter*
  than the feature twin's the owner already accepted — so the `uirestore` exclude list stays as
  G15-0 ruled it. Two facts it forced into the record: **`e->wrap` is decided once, when a frame
  is first atlased, and a frame first seen while the palette is uniform keeps a tileability flag
  the settled palette would not produce** (one entry of 34 in one shell run; restored the wrong
  way it is 10 levels off over half its texels — the fix is to recompute it on the re-arm, not
  taken here); and **the presented palette was `min(255, (int)(entry × 1.125))` on the instances
  measured**, so `paldiff=235` was the ordinary reading and every pass still on `main+0x143A7`
  drew the world ~11 % darker than the engine presents its own pixels. **The attribution to "the
  template wine prefix carries `Gamma = 15`" is withdrawn** ([GL UI renderer](gui-renderer.html)
  §15, measured 2026-09-09): the template and all 58 instance prefixes are **one inode**, wine
  rewrites it in place at launch, it now reads **12**, and instances launched under it present
  `paldiff=0` — no seam. The mechanism is unchanged; the *value* is shared and mutable, so read
  `paldiff=` rather than assuming it.
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
  **Since G18-8 that surface is retired at the free itself** rather than at the next
  `0x4C69F0("OFFSCREEN")`: the observer on `MEM_Free 0x4D85A0` is the surface's destructor, so
  no entry in the table can outlive its block whatever path freed it. That is the whole safety
  argument for `pub_surface_bytes` reading engine memory at the flip — the range test on the
  pointer was never one. MEASURED 2026-09-12: the two frees the old rule left standing are the
  shell's 640×480 offscreen at the mode switch (`0x498398`) and the game's at leave-game
  (`0x491ABD`), both on the game thread.
- *The palette (G15d):* the twin resolves through **the palette the engine's frame is presented
  with — cnc-ddraw's `g_ddraw.primary->palette->data_rgb`, what the engine's `SetEntries`
  stored — not `main+0x143A7`**. The engine scales every palette it sets by the Gamma option on
  its way to DirectDraw (`0x4BA200`, `SetGamma 0x4BA590`; engine map, "The palette the screen is
  presented with") and never scales its own table, so at Gamma ≠ 12, or after `+gamma N`, the
  two differ and every `+0x143A7` reader — the world passes — is off by the factor. The read is
  taken under the fork's `g_ddraw.cs` (the game thread NULLs the primary inside it); the
  engine's table is the fallback until a primary exists. The heartbeat carries `palchg=` (uploads
  — a fade is a run of them) and `paldiff=n@i` (entries where the presented palette and
  `+0x143A7` disagree, and the first): 0 in game, 1 (index 9) in the shell, **235 (from index 1)**
  at `+gamma 15` — the count `uiwalk.py` records and this page had as 255 until 2026-09-09.
  **Since 2026-09-09 this is not the UI layer's own code**: the resolution moved into
  `tagpu_pal.c` and serves the world's passes too (§2.3f), which is where the same-frame
  disagreement it measured actually was.

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
**G17a (2026-09-09) did not change that**: it added a GL texture, an FBO and shader arithmetic
and reads no engine address the module did not already read. **Nor did G17b**, which is
fork-side and tooling: the client-area → engine-logical pointer transform moved out of
`wndproc`'s button cases into `mouse_client_to_game` (`mouse.c`) so the harness's device-space
click takes the same path a player's does, the UI snapshot gained the frame's `viewport`, and the
native pass gained `devres` — at `k > 1` `ss` follows `ceil(k)` and the box-resolve to the game's
resolution is skipped, so the composite downsamples the supersampled buffer instead of
nearest-stretching a game-res one (replication 42.6 % → 11.2 % of adjacent device pixels at
`k = 1.5`, fps unchanged). **It is OPT IN — `tagpu_devres.on`** (it was on by default with
`tagpu_devres.off` as the A/B until the rect measurement above; `devres=` is on the native log
line either way); at `k = 1` it is inert.

### 2.3f The palette the world resolves through (`tagpu_pal.c`, always on) — 2026-09-09

`main+0x143A7` is the engine's own palette table and **it is not what the screen shows.** Every
palette the engine sets goes through `0x4BA200`, which keeps the entries in the graphics globals
and hands DirectDraw `min(255, entry × *(float*)(globals+0x614))` — the Gamma factor
(`SetGamma 0x4BA590`; an option screen computes `0.5 + Gamma/24`, `+gamma N` in chat sets `N/10`
outright) — and never scales `+0x143A7` ([engine map](exe-reverse-engineering.html), "The palette
the screen is presented with"). G15d made the UI twin resolve through the presented palette and
left the world reading the engine's table, so at any factor but 1.0 the world was the wrong
brightness beside the engine's own pixels **in the same frame** — and the reference setup's own
instances run at 1.125, because their registry Gamma is 15.

`tagpu_pal.c` is the one resolution of that question for the whole DLL. `tagpu_overlay_draw`
marks it stale once per present and the first reader re-resolves it — the primary's palette
object (`g_ddraw.primary->palette->data_rgb`, what cnc-ddraw's `ddp_SetEntries` stored) read
under the fork's `g_ddraw.cs`, the engine's table the fallback until a primary exists — into a
1024-byte snapshot of ours. **The lifetime is the fork's lock, not a probe**: the game thread
NULLs `g_ddraw.primary` inside that section and frees the object only after leaving it, so a
pointer read *and dereferenced* inside it is a live object or NULL, never a freed one
(`CLAUDE.md`, *Fixes must be safe by construction*). The snapshot being ours also means the
atlases and restorer jobs that keep a palette pointer no longer hold one into engine memory,
which they did before.

- **The Classic half is one upload.** The world has exactly one palette texture —
  `tagpu_native.c`'s `s_palTex`, handed on to the terrain, feature, effect, marker and
  replacement-mesh passes as `uPal` — so the indexed path is fixed by that upload taking
  `tagpu_pal_live()`. It is **exact**, not approximate: our Classic passes do their arithmetic in
  index space (the SHD row, the fog LUT) and look the palette up last, exactly as the engine
  does, so resolving through the scaled palette *is* the engine's `min(255, gamma × pal[i])`.
- **The Classic++ twins are baked, so they go stale when the palette moves.** Each records the
  serial it was restored through; when that moves, the atlas re-points its job's palette
  (`tagpu_rglsl_job_repalette`) and queues every entry again **without clearing the destination**
  (`tagpu_rglsl_job_repaint`), so the world recolours cell by cell instead of blanking for the
  length of the job. The re-arm is gated on the job being **idle**, which bounds it to one
  repaint of an atlas in flight however often the palette moves — a bound, not a rate limit.
- **A replacement mesh's colour never came from a palette**, so resolving through a different one
  cannot reach it: `tagpu_hires_draw.c` takes `tagpu_pal_gamma()` as `uGamma` and applies the
  engine's own `min(255, c × gamma)` to its fragment instead.
- **What still wants the engine's table wants it because it is unscaled.**
  `tagpu_rglsl_tileable`'s threshold is a raw colour distance and a scaled palette stretches
  every distance by the same factor, so it takes `tagpu_pal_engine()` and the classification
  stays a property of the ART. Measured on Two Continents' 5062 terrain tiles: 177 wrap-padded at
  factor 1.5 against 400 at 1.0 while it read the presented palette; **400 at both** after.
- **`tagpu_order.c`'s `seq_ink` is the one reader that stays on `+0x143A7` directly**, and the
  reason is the thread, not the colour: the order walk runs on the GAME THREAD
  (`tagpu_order.h`, "the two-thread split") while this module resolves on the render thread's
  cadence, so a call from there would race the snapshot every other pass reads. It costs nothing
  to leave it — the ink it picks is the argmax of a luminance ranking over one sprite's own
  colours, and scaling every entry by the same factor cannot move an argmax. **Everything in
  `tagpu_pal.h` is render-thread only**, and that is the reason it is.

**MEASURED 2026-09-09** — Two Continents, `scenarios/tascene-parity.json`, camera parked on open
ground at 1024×768, Classic (`classicpp` off), the pass's own A/B: `terr.on` (ours) against
`terr.on=passive` (the engine's own terrain, in the same frame, through the presented palette).
The "before" column is HEAD's DLL built into a scratch tree and pinned with `--keep-dll`, so both
columns are the same scene at the same factor:

| | differing px of the 896×703 viewport |
|---|---|
| **before**, `+gamma 15` (factor 1.5) | **583 010 — 92.6 %**, mean RGB 0.67 / 0.73 / 0.84 of the engine's |
| **after**, `+gamma 15` (factor 1.5) | **19 419 — 3.08 %** |
| **after**, factor 1.125 (registry Gamma 15) | **19 419 — 3.08 %** |
| **after**, factor 1.0 (the stock default) | **19 419 — 3.08 %** |

Those 19 419 are the engine's own tree sprites, which this measurement deliberately leaves to it
(`feat.on` unarmed, and our terrain covers what it drew): the **same** pixels differ at every
factor and the terrain around them is byte-identical, which is the whole claim. The other
direction says the same thing from inside: our own frame at factor 1.0 against 1.5 differs on
99.99 % of the viewport at a mean ratio of 1.115 / 1.118 / 1.109 — **before the fix those two
frames were the same picture.**

The Classic++ repaint measured on the same map: `terr: palette changed (serial=N): 5062 tiles
queued for repaint`, then a full second restore in 2.5 s at 57 fps, with the visible cells (which
restore first — `restore_order` ranks from the centre of the viewport) correct within two
seconds and **no blank frame**: a `glshot` taken 2 s into the repaint already carries the new
brightness and zero black pixels. The heartbeat's `palchg=`, `paldiff=n@i` and `palsrc=` now come
from this module rather than the UI layer's own copy, so the numbers `uiwalk.py` reads are
unchanged, and `tagpu.log` gains one `pal: presented palette changed (serial= src= diff= gamma=)`
line per change, rate-limited to one a second so a campaign fade cannot flood it.

### 2.4 Tooling (not part of the render path)

| VA | What it is | Module (arm file) |
|---|---|---|
| `0x45AC20` | `DrawUnit` entry (7 stolen) | `suppress` (`suppress.on`), `tracer` (`tracer.on`) |
| `0x459200` | composite sprite → screen blit | `tracer` |
| `0x469A05` / `0x469BA8` | the two `DrawUnit` return sites | `tracer` |
| `0x4969D2` | the sim tick site (5 stolen) | `scenario` — applies a situation on the game thread |
| `0x4B08C0` `0x4B0DA0` `0x4B19D0` `0x4B1A99` + the `E8` at `0x4B15E0` | the COB engine's thread allocator (5 stolen, wrapped: the original runs through the stolen prologue, then the start is latched), the thread runner's entry (5), the `RETURN` handler mid-function (5), the `signal` kill mid-function (6), and the `rand` handler's call into the sim RNG `0x4B6C30` (call-site redirect that calls it itself) | `cobtrace` (`tagpu_cobtrace.on`, its contents a unit-type filter) — the COB script-call oracle for tacob: S/R/X/K/D lines to `tagpu_cobtrace.log`, stamped with the sim tick `main+0x38A47`; reads only, game thread only; all five or none. `tagpu_posedump.on`'s header line now carries `tick=` and `idx=` so the two logs join, and since 2026-09-08 `body=` (the cached triple the compose folds) and `live=` (`unit+0x68/+0x66/+0x64`) beside it, both in axis order — [exe-reverse-engineering](exe-reverse-engineering.html) §"The COB engine", [tacob-design](tacob-design.html) §"The trace contract" |
| — | `tagpu_posecrc.on`: one line per unit per SIM TICK carrying `in=` a CRC32 of every byte `posed_pose` reads and `out=` a CRC32 of every byte it writes, plus `raced=`. **The only oracle that watches the matrices the unit pass hands the GPU** — `posedump` dumps the *engine's* fields and `tacob pose-check` diffs tacob's reconstruction of them. It is joined on the INPUT, never on the tick, so two runs need no tick-for-tick determinism: `posed_pose` is a pure function of its input, so a shared `in` must carry the same `out`. The input is hashed on **both sides** of the pose loop and the sample is dropped and counted when the game thread moved a field under it (§2.9's residual, measured at 0–0.33%) — [smooth motion](smooth-motion.html) §7e | `tagpu_native.c`, no engine address |
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
| `main+0x1431F` / `+0x14323` | eyeX / eyeY. **WRITTEN, on the GAME THREAD only since 2026-09-12** (§2.17): the command apply at the top of every in-play draw steps it by the cursor anchor's delta, holds it where `tagpu_eye.txt` says, and re-applies the camera range (§2.3c) so a zoom-out cannot leave it past the bounds — every value written goes through `clamp_pair()` into the range at the commanded level. The render thread reads the eye from the frame packet and draws from it plus the deltas not yet acknowledged; it never reads or writes the field. Sim-neutral for the same reason `ScrollSpeed` is |
| `main+0x14327` / `+0x1432B` | `MapXScrollingTo` — where the camera is heading; the stepper `0x41CA10` eases the eye toward it. **WRITTEN by the command apply, on the game thread, always together with the eye** and clamped to the same range, because a disagreement between the two is what the stepper reads as a camera move in flight and would cost the fog grid its is-current flag every frame (§2.3c). The replacement clamp deliberately does **not** touch it — three of its callers are inside the stepper, and writing the target there would stop the camera ever arriving |
| `main+0x142CB` | the minimap's view RECT. Engine-drawn and engine-filled — `0x41C3C0` is the only place it is computed — so the command apply recomputes it through the same wrapper on the draws it moved the eye. **Game thread since 2026-09-12**; it was the one render-thread write of it before (a one-frame torn box while the game thread drew the minimap) |
| `main+0x14281` bit 3 | the screen fog grid's is-current flag. **CLEARED by the command apply after any eye it moved, on the game thread** — the same clear the engine's own eye writers make at `0x41CB6B`, and safe only there: `0x484904` sets it with an unlocked read-modify-write, so a clear from the render thread could be swallowed ([engine map](exe-reverse-engineering.html), "who may clear `main+0x14281` bit 3"). Until landing 2 the render thread asked terrown's fog tick for the rebuild through a request/ack pair instead; that handshake is gone |
| `main+0x37E27..0x37E3B` | viewport rect: L, T, R, B, then W, H. **L/T/R/B are WRITTEN while `vpwide` is live, on the game thread since 2026-09-12** (§2.3b, §2.17): the command apply derives the widened rect from the level the record carries and restores the true one when nothing is zoomed. The true 1× rect reaches the render thread as the packet's `vp`; `tagpu_vpwide_true_rect()` is game-thread only now. W/H are never written; a disagreement with the screen-derived size is counted (`vpwh=`), not repaired |
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
| `main+0x1426B`, `+0x142CB`, `+0x142DB`, `+0x142DF`, `+0x142E3`, `+0x142E7..+0x142ED`, `+0xDD9` | the minimap: the TNT's picture, the view rect and its colour, and the three 126-px surfaces (composite, fog base, scaled base). **Read only.** The picture is decoded on the MINIMAP BUILD's own thread inside `BuildMinimapSurface 0x466780` — not the game thread, measured — and the three surfaces are read per frame on the render thread while the game thread may be rewriting them, the same standing as the fork's own surface upload (G17e, [GL UI renderer](gui-renderer.html) §19) |
| `[0x51FBD0]+0x1B2`, `+0x1B6`, `+0x1BA` | the cursor's **GAF frame** and the position it was last drawn at. Read only, on the render thread, once per frame in `tagpu_gui_cursor_frame()` — and read ONCE because two modules act on the answer: the UI layer stops discarding that rect and the world composite counts it as the terrain key, and a second read a pass later would leave a sliver of the engine's cursor standing (G17c, [GL UI renderer](gui-renderer.html) §17). The frame's pixels go through `tagpu_gaf_decode` into the UI atlas like any other sprite |
| `[0x51FBD0]+0x204` / `+0x208` | the current font object and text foreground colour. Read only, on the GAME THREAD at hook 8: the engine re-points both many times a frame, so a present-thread read would get whatever the side panel last drew with. **Since 2026-09-12 (the frame packet's landing 1) the font is COPIED there**, header and 95 printable glyphs, each as a one-glyph font object, into the packet (`tagpu_packet_pub.c`); the present thread rasterises from the copy and no longer dereferences the engine's font at all (`tagpu_text.c`, §2.16). The GL UI's string op still carries the font's address — landing 4c |
| **the frame packet's header and its four world tables** — the header: `main+0x38A47` (`GameTime`), `+0x38A4D` (the live speed), `+0x38A51` (paused), `+0x38D75` (the load flags), `+0x1431F`/`+0x14323` (eye), `+0x14327`/`+0x1432B` (scroll target), `+0x37E1F`/`+0x37E23` (screen), `+0x37E27..+0x37E33` (the rect the engine can name, since landing 2), `+0x1422B`/`+0x1422F`, `+0x14233`/`+0x14237` (map px, map cells), `+0x1423B`/`+0x1423F` (view cells), `+0x1438F` (`UNITINFOCount`), `+0x14351` (unit slots), `+0x14281` (`LosType`), `+0x37F06`, `+0x37F2F`, `+0x2A43`, `+0x2A42`, `+0x1427F`, `+0x143A7` (the palette table, 1 KB, since landing 2), `[0x51FBD0]+0x614` (gamma, bounded, since landing 2); **since landing 3** also `+0x0DCB` (the GUI colour array), `+0x2C76`/`+0x2C7A` (the dispatched mouse point), `+0x2C92..+0x2CA6` (the build cursor's two corners), `+0x2CC3`/`+0x2CC6` (the cursor mode and region flags), `+0x1424B`/`+0x1424F` (the feature sweep), `+0x14253` (`NumFeatureDefs`) and `[0x51FBD0]+0xC4` (the 32×256 shade table). **The three per-map BASES are deliberately NOT in it** — `+0x1426F` (FeatureDefs), `+0x1420B` (wreck records) and `+0x14377` (`MODEL_PTRS`) are read live, at every use, on the render thread: the teardown frees each and then NULLS it (`0x4221F8`→`0x422214`, `0x42227D`→`0x42228B`, `0x42DCCB`→`0x42DCD8`), so the null is what refuses the walk, and a copy taken at publish time and held for a frame reads straight past it. **[CORRECTED 2026-09-12 by a landing review, which found the copies.]** The tables: the unit array walked to `+0x14351`'s count, each record's `+0x64..+0x110` fields, its `UnitDef`'s `+0x20`/`+0x1FA`/`+0x241`, its `Object3do`'s `+0x00`/`+0x10`/`+0x18`/`+0x1E` and every `+0x22 + i·0x36` piece, the feature grid `+0x14287` over the widest zoom rect, and the wreck records the anchors name ([engine map](exe-reverse-engineering.html), "What the frame packet's publisher copies") | **Read only, on the GAME THREAD**, from the `after` of the `DrawGameScreen` observer on in-play frames only, and COPIED into the packet every presented frame (§2.16, §2.17; the addresses live in `inc/tagpu_engine.h`). **Since landing 2 the packet's `vp`, `eye` (plus the unacknowledged anchor deltas), `vp_addr`, `pal` and `gamma` ARE the view every pass draws from** — the native pass, the scaffold, the marker pass's build-cursor gate, the GL UI's layer draw and the palette module read no engine field for any of them |
| **order node `+0x32`, `+0x34`, `+0x42`** | **the target sprite's last-seen cache. WRITTEN, on the GAME THREAD, at the instant the engine's own drawer would have written it.** It is the only sim-side field this stack writes for a marker, and it is not optional: the cache is what stops a waypoint marker following a target that has left LOS, so a port that drops it leaks the target's live position (`tagpu_order.c`, `resolve_sprite`) |
| **`Object3do+0x08`** | **the pose-dirty flag, and the interlock the unit pass reads it as.** Read only, on the render thread, on either side of every piece's posed-vertex copy: the engine rewrites `prim+0x22` in place and in two stages, and this field is 1 for exactly that window ([engine map](exe-reverse-engineering.html) "The repose"). Non-zero on either side means the buffer may be mid-rewrite and the pass emits the piece from the pose fields instead (§2.9) |
| `Object3do+0x18/+0x1A/+0x1C` | the CACHED body turn — `unit+0x64` (about Z), `unit+0x66` (the heading, about Y), `unit+0x68` (about X), copied at `0x45AC7C` when any axis moves ≥ 8. Read only, and read in preference to the live `unit+0x64..` on the reconstruction path, because this copy is the one the compose baked into the vertices. **`[MEASURED 2026-09-08]` "In preference" is not a nicety: on a bomber the cached triple read `(0, 16128, 3)` against a live `(0, 44767, 65508)` — 157° of heading apart — and the drawn geometry followed the CACHED one.** On a tank the two were identical; which of them moves is not established. Anything folding `unit+0x64..` instead draws the unit at the wrong attitude, which is what `pose_dump` and `tacob pose-check` did until 2026-09-08 and `hires_pose` until 2026-09-09 |
| the **level generation** (the frame packet's `level_gen`) | not an engine field — our own counter, bumped on the game thread at every level end and carried to the render thread inside the packet. It is how a cache keyed on a **model template** pointer (`s_aabb`, `s_sbox`, `s_pmap`, and the geometry bake's) learns the level ended: the template tree is shared by every unit of a type and is NOT freed through `FreeObjectState`, so the deferral covers units and not it. Before 2026-09-08 nothing dropped those three at all — a second level reusing an address served the first level's answer, silently, for the life of the process ([thread-safe destruction](thread-safe-destruction.html) §6a). **[CORRECTED 2026-09-12, a landing review]** between then and landing 3 the counter read was `tagpu_reclaim_level_gen()`, which is bumped only in reclaim's teardown post hook — so under `tagpu_reclaim.off`, or any of reclaim's four other ways not to arm, it never moved and the caches were exactly as stale as before 2026-09-08. The publisher owns the counter now and advances it whichever provider publishes the level-end packet |
| the **pose history** (`tagpu_lerp.c`, `tagpu_lerp.on`) | not an engine field — our own arena, 3.4 MB of `P_POS`/`P_TURN` snapshots keyed by `(Object3do, nparts, level generation)`. **It READS `pr+P_POS` and `pr+P_TURN` and writes NOTHING back**, which is the whole safety argument: the sim reads those fields (`get PIECE_XZ`, and `QueryPrimary`/`AimFromPrimary` hand the engine weapon muzzle origins out of them) and TA has no runtime desync detection, so a framerate-dependent per-machine blend written there would diverge two machines silently. Verified by running it: the walker's COB trace is byte-identical with the lever on and off ([smooth motion](smooth-motion.html) §7g) |
| `node+0x24` | the model's REST vertices, `count × 12` bytes of 16.16. Read only. Shared by every unit of a type and never written after load, which is what makes the reconstruction in §2.9 safe to build from while the engine is rewriting the posed copy |
| `main+0x1421F` | the screen fog grid `{u16* buf; cols; rows; cells}`. Read only, per frame on the render thread. **Its last column and last row are short their outer corners** — the map cell that would supply them is past the builder's loop — so a sampler that clamps a world point into that cell reads *no fog*, not the border cell; `taFog` clamps to `uFogDim − 1.0`, one whole cell short, and the grid's own overshoot of the viewport — at least 1 px on every side for every viewport size the allocation accepts and every eye, 16 px for a negative one — is what makes that a no-op at 1× (terrain-depth §8a). **`cells` is the ALLOCATION**, `(cols*rows + 7) & ~7` — asserting `cells == cols*rows` accepted 1024×768 and refused 1920×1080, where the refusal cleared `fogMode` and there was no fog at all until 2026-09-09 ([terrain & depth](terrain-depth.html) §5.2). **The dimensions are one cell per 32 px of the 1× viewport plus two**, whatever the zoom: MEASURED by `tacli peek` through this descriptor, **118 × 68** for the 3712 × 2096 viewport of a 3840×2160 screen (`cells` 8024, exactly the product) and **78 × 45** for the 2432 × 1376 of a 2560×1440 one (`cells` 3512 against a product of 3510 — the round-up, and the case that refused). The `cols`/`rows` sanity bound in `tagpu_native.c` is 1024, not 256: at that rate 256 is a viewport 8128 px wide, which made the bound a screen limit standing in front of the real test |
| `main+0x2A43`, `main+0x1B63 + id*0x14B + 0x7C`, `main+0x14273`, `main+0x14233`/`+0x14237` | the LOCAL player id, that player's LOS counter block `{u8* buf; w; h}`, the MAPPED bitmap (u16 per tile, one bit per player, row stride `PLOT_C` **bytes**) and the PLOT dimensions. Read only, **on the GAME THREAD** from `tagpu_fogwide.c` at the fog overlay's own call site — which is the lifetime argument for reading them at all: the engine's builder reads the same two allocations there. Every index is bounded by the dimensions read alongside them |
| `main+0x37F06` bit0 | `damagebars` registry option |
| `main+0x37F06` bit2 / bit3 | the graphics options `Shadow` / `TShadow` (the blit tests `al,4` at `0x45928E`, [shadows & cloak](shadows-cloak.html) §2). Read only, per frame. The Classic silhouette needs both, the slant only bit2 — and **since G14i bit2 also gates the Classic++ shadow map** (with `shadows=1` in the cfg), so the player's in-game Shadows toggle keeps its meaning under the switch; bit3 is ignored there |
| `main+0x37F2F` bit2 | `SelBoxes` |
| `main+0x142E7..0x142ED` | minimap rect on screen |
| `main+0x1423B` / `+0x1423F` | view size in map cells (the minimap rect's size comes from here) |
| `main+0x14283` / `+0x1428B` | `TILE_SET` `{count, pixels}` and the `u16` `TILE_MAP` — the terrain pass reads both per frame. The GLSL restorer reads them once more when its job starts, on the render thread: each tile's edge texels for the tileability test and the whole tile map once, to rank tiles by distance from the centre of the viewport (`tagpu_terr.c` `restore_order`, G14e: from the centre, so the reveal radiates); the pixels themselves are sampled from the R8 atlas already on the GPU |
| `main+0x143A7` | the engine's own palette table, 256 × (R, G, B, pad). Read only, **on the game thread by the frame packet's publisher since 2026-09-12** (a 1 KB copy in every packet, the level-end one too), and by nothing on the render thread: `tagpu_pal.c` takes it from the packet — **and it is not what the screen shows** (§2.3f). Every pass that turns an index into a colour takes `tagpu_pal_live()`: the world's single `uPal` upload, and the restorer's per-job snapshot for the terrain, the feature and effects atlases (`tagpu_gaf_atlas_restore`, per frame) and the unit atlas (`tagpu_r3d_atlas_frame`, per frame before the first UV lookup). Two readers still want the unscaled table and say why: `tagpu_rglsl_tileable`'s threshold is a raw colour distance, so it must classify the ART and not the display (`tagpu_pal_engine()`); and `tagpu_order.c`'s `seq_ink` wants the engine's own table, because a luminance ranking over one sprite's colours cannot be changed by a uniform scale of all of them — it takes it from the **packet's copy** since landing 3. **[CORRECTED 2026-09-12]** the reason given here until then, that `seq_ink`'s walk is on the game thread and must not touch a snapshot the render thread resolves, was simply wrong: `tagpu_order_gather` is called from `tagpu_mark_gather`, inside the unit pass, on the RENDER thread — only the order-marker snapshot is game-side. The second reason is the real one and is unaffected |
| `*(0x51FBD0) + 0x614` | the gamma factor `0x4BA200` multiplies every palette entry by on its way to DirectDraw (`SetGamma 0x4BA590`). Read only, **on the game thread by the packet publisher since 2026-09-12** (a bounded float in every packet; `tagpu_pal_gamma()` answers from the copy), and only for the one thing a palette cannot reach — a replacement mesh's glTF texture, which never came from a palette at all (`tagpu_hires_draw.c`'s `uGamma`, §2.3f). **Bounded** to 0.05..8.0 against the slider's own 0.5..1.5 and the chat command's N/10; anything else, NaN included, reads as 1.0, the identity |
| `main+0x14287` | the `FeatureStruct` grid, one 13-byte record per 16-px cell, `mapW16 × mapH16` (`main+0x14233`/`+0x14237`). Read only. `tagpu_feat.c` reads the height byte (`+0x04`) of the anchor's four corners per anchor per frame for the engine's own projection, and since G14f the four central-difference neighbours too, for the ground's lambert (Classic++ only); `tagpu_terr.c` (G14f) copies the height byte of **every** cell once per map, when it builds the atlas, into an R8 texture the terrain shader samples — keyed on the grid pointer, the dims and the tile set, re-checked every frame; the grid is `IsBadReadPtr`-checked whole before the copy (7 MB on Two Continents), and an unreadable grid leaves Classic++ terrain **unlit** (the restored colour and the grey rule stay, the lambert is skipped), logged and retried every 60 frames |
| **`main+0x1434D`** | **`ScrollSpeed`** — sim-neutral (a local camera preference no other machine ever sees), driven at base/z **by the command apply on the game thread since 2026-09-12** (every in-play draw, before the next frame's scroll poll reads it; the base restored at the level end), and its save path is guarded (§2.3) |
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
| the model templates' two frees, **`0x42DC01`** and **`0x42DCB6`** (inside `0x42DB90`, whose one caller is the teardown cascade) | **call-site redirect**: each 5-byte `call MEM_Free` rewritten to call ours, after checking the site really is `E8` with a rel32 resolving to `0x4D85A0`; both or neither, and the rel32 is computed against the call site rather than the buffer it is built in | a template block — one allocation carrying the whole tree, its rest vertices and its faces — and the pointer table are **enqueued** on the same ring instead of freed. Released by the post hook below whenever the pre hook proved the reader idle, which is the guarantee rather than any elapsed time; on the timed-out path they fall back to the epoch stamp. This is what stops the pre hook's timeout from being a safety argument — [thread-safe destruction](thread-safe-destruction.html) §6c. **Five `MEM_Free`s live in that body, not four**: `0x42DC23` and `0x42DC52` are unit-def fields no pass of ours reads, and `0x42DCCB` frees the whole UnitDef array at `main+0x1439B` — which we *do* read (`tagpu_order`, `tagpu_cat`, `tagpu_weapons`, `tagpu_scenario`), safely only because every reader and this cascade are on the game thread |
| level teardown `0x491B60` | **wrap**: `pushad; call pre; popad; call <stolen tail>; pushad; call post; popad; ret` (no stack args; two exits — `ret` at `0x491C59` and a tail-jump to `0x450DD0`, which also takes nothing and returns with `ret` — so its body can be called and returns to us either way) | pre: raise a flag (fenced), wait ≤ 1 s for the render thread to leave its pass; if it does, **flush** the queue through the real destructor while the composite registry it walks is still alive and let the cascade (every unit, through `0x485980 → 0x4864B0 → 0x4866D0`, plus the wreck loop) free synchronously; if it does not, **nothing is freed** — the queue is kept and deferral stays on through the cascade (`held=`), draining at the next game's first deaths; post: free every block the cascade queued if the reader was proved idle (one `reclaim: teardown post: freed N block(s) …` line), bump the level generation, deferral back on, flag down |

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

### 2.8b The fork defaults (`tagpu_cfg.c`) — since 2026-09-10

`tagpu_opt.c` arms our passes with no file; this is its sibling for **cnc-ddraw's own settings**,
so that a player never edits `ddraw.ini` to get a working game. It runs at the END of `cfg_load()`,
immediately before `ini_free()` — the parsed ini is still in memory there, which is what lets it
ask whether the player wrote a key, through the same section rule `cfg_get_string` uses (the game
section, then `ddraw`).

| key | ours | why |
|---|---|---|
| `windowed` + `fullscreen` | both true | borderless fullscreen (`dd.c` sizes the render target from the desktop mode); **one decision, not two** |
| `toggle_borderless` | true | alt+enter switches borderless ↔ window instead of taking `util_toggle_fullscreen`'s exclusive branch, which is a real `ChangeDisplaySettings` |
| `max_resolutions` | 90 | the mode list TA is fed; **and bounded at 100 whatever the ini says** |
| `inject_resolution` | the desktop mode | the one list entry exempt from the `CDS_TEST` filter, so the monitor's own mode is *guaranteed* into the picker ([resolution](resolution.html) §6.5) |

**A key the player wrote wins**, and that includes the one `cfg_save()` writes back after they
press alt+enter — so their own choice sticks across launches. One line at attach says which way it
went: `cfg: tagpu defaults: max_resolutions 0 -> 90, toggle_borderless 0 -> 1, windowed 0 -> 1,
fullscreen 0 -> 1 (the ini wins; the player set nothing)`.

Four things about it are load-bearing and were each found by measuring rather than by reading:

- **`windowed` and `fullscreen` are atomic.** `cfg_load` defaults `windowed` to FALSE, so owning
  `fullscreen` alone would give a player with no ini fullscreen-*without*-windowed — the exclusive
  modeset the `toggle_borderless` row exists to prevent. If the player wrote **either**, we own
  **neither**. Verified: an ini with only `windowed=false` logs `the player set windowed,
  fullscreen` and applies neither.
- **The bound is not a default.** `max_resolutions=0` means *no cap*, and TA's "DISPLAY MODES"
  allocation is 100 entries whose writer does not bounds-check ([resolution](resolution.html)
  §6.1/§6.3). The clamp therefore applies to the player's own value too, and says so:
  `cfg: max_resolutions 250 -> 100 (TA's mode buffer is 100 entries and its callback 0x4B5330
  does not bounds-check)`.
- **We had to stop shipping these keys, in two places.** `cfg_create_ini()` writes a full ini for a
  player who has none, and its **generic `[ddraw]` block** set all four — so every key read as
  "the player's" and *nothing applied*. That is exactly what the first end-to-end run showed
  (`the player set max_resolutions, toggle_borderless, windowed, fullscreen`, all four skipped).
  They are commented out there and gone from `[TotalA]` and from `tagpu/release/ddraw.ini`. **If
  one comes back, this module silently stops working and nothing warns you.**
- **No display API at `DLL_PROCESS_ATTACH`.** `cfg_load` runs under the loader lock, so
  `inject_resolution` is filled lazily inside `EnumDisplayModes`, where the desktop mode has
  already been read for `max_w`/`max_h`.

**tacli is unaffected and deliberately so:** `write_ddraw_ini` writes all four explicitly, so an
instance is always explicit and every measurement is unchanged — which also means **an instance
does not exercise the player path**. To test that path, put `tagpu/release/ddraw.ini` in the
gamedir (or delete it entirely and let the DLL create one) and launch bare, with no `--res`,
`--window` or `--maxfps`, which are the only knobs that rewrite the file.

### 2.9 The pose race, and the guard that closed it — HISTORY (removed by G16 step 8, 2026-09-09)

> ⚠ **None of this is behaviour any more.** The guard, the rest-equality detector, the
> reconstruction, `posewatch` and the levers `tagpu_posefix.off` / `tagpu_posewatch.on` /
> `tagpu_poserecon.on` were deleted with the CPU emitters that were the only things that reached
> them, and the `native:` line's `posefix=` / `guard=` / `rest=` / `norecon=` / `errmax=` fields
> went with them. **Nothing reads `prim+0x22` now** — §2.11 builds the pose from the FIELDS — so
> the race described below cannot happen rather than being detected and worked around.
>
> The section is kept whole because it is the evidence, not the behaviour: it is where the
> artifact was characterised, where the engine's two-stage repose was established, and what every
> later gate was measured against. Read it as the record of a bug that no longer has a mechanism.
> What replaced each of its parts is [GPU posing](gpu-posing.html) §4's refusal ledger; what is
> left of the residual is named at the end of §2.11.


**The bug.** A walking commander showed one-frame pops: for exactly one presented frame the unit
was drawn in its **unrotated rest orientation** — upright, front-on, no body yaw — and then
snapped back (~1400 changed pixels at 2× zoom, three frames of a 62-second walk). Zoom is an
amplifier and not the cause: the event *rate* is the same at 1× and 2×, and zoom multiplies each
event's on-screen size about fourfold in pixels.

**The cause, and why nothing else can produce it.** The engine rewrites every posed vertex buffer
`prim+0x22` **in place, on the game thread, in two stages** — first `rep movs` of the node's rest
vertices back over it for the whole piece tree (`0x45ACC1`, `0x45B030`), then the compose that
puts the piece turns and the body turn into them (`0x45B0A0` → `0x45B150`), one vertex at a time.
The native unit pass gathers on the **render thread** and reads that buffer live. The only code in
the engine that ever writes rest vertices into `prim+0x22` is that reset, so a frame that draws
the unit at rest is a frame that read between the two stages. Full derivation and every address:
[engine map](exe-reverse-engineering.html) "The repose, and the window it leaves open".

**The interlock.** `Object3do+0x08` is set to 1 before the reset and cleared only after the
compose returns, and the rewrite is entered only when it is non-zero — so it brackets the window
exactly. `emit_geom` and `emit_slant` read it on either side of each piece's vertex copy (two
`int` loads and a compiler barrier; x86 does not reorder loads with loads, so nothing stronger is
needed). If it was set on either side the whole unit is re-emitted from the **pose fields** —
`pose_accum`'s reconstruction, with **all three** cached body words folded into the base
piece's turn the way `0x45B0DB` folds them (`pose_dump`'s `err=` is built from the same
reconstruction since 2026-09-08, where it used to apply the heading alone — though not the same
number: `recon_err` skips pieces whose visible bit is clear and compares `recon_prim`'s
16.16-rounded output, `pose_dump` reports every piece including hidden ones, in float) — written into the same 16.16 representation the engine's buffer holds, so
both emit paths consume it with the arithmetic they already had.

The flag is *also* 1 while the buffer is merely **stale** (a COB `move`/`turn` the next `DrawUnit`
has not composed yet), which is most of what trips the guard and would have been safe to draw.
Nothing in the struct tells the two apart, and the reconstruction is the pose the engine is on its
way to, so the guard does not try: it takes the fields whenever the flag says the buffer might be
moving.

**Why the fallback is not itself a change.** Forcing it for every unit on every frame
(`tagpu_poserecon.on`, a measuring lever) renders **byte-identical** to the engine-buffer path —
0 differing pixels of 1920×1080 on a parked commander, and the same change bbox as two
consecutive engine-path shots on a scene with a spinning radar dish and a solar collector, i.e.
the dish's own motion and nothing else. The oracle below reads the disagreement between the two
as `0.00` model units in every five-second window that contains no trip.

**The evidence, from inside the DLL.** `tagpu_posewatch.on` arms an oracle that, once per unit per
frame, rebuilds the pose from the fields and reports the largest disagreement with the engine's
buffer in model units, with the frame number and the dirty flag read on either side. A stale
buffer reads a unit or two out; a buffer caught mid-rewrite reads the model's own size out — the
readings run to the model's own size — 34.00, 38.63, 34.16, 33.03, 32.96, 32.82, 32.69,
32.00, 25.23, 23.18, 22.24. **Every one of them had the dirty flag set on both sides**, which is
the property the guard depends on, and the guarded pass reported the same event on the ones it
sampled. `tagpu_posefix.off` leaves the guard measuring and draws the engine's buffer anyway,
which is the baseline the fix is measured against; the guard deliberately finishes its walk rather
than bailing at the torn piece, so that baseline is the emission the pass made before it existed.

⚠ **`[2026-09-08]` Gate A ran that oracle over a 69-unit screen inventory, and two things came
back.** ([GPU posing §0b](gpu-posing.html), the numbers in
`research/notes/evidence/posewatch/gate-a-2026-09-08.txt`.)

1. **The guard's bracket holds over everything, not just the commander.** 27142 `posewatch:` lines
   across all eight classes and both extremes of stock geometry, and **every one of them reads
   `dirty=1/1`** — not one large `err` with the flag clear on both sides, which is the one reading
   that would say the bracket is not the whole window. `norecon` is 0 in every window, so no
   model's piece tree failed to rebuild either.
2. **A handful of STRUCTURES sit at rest permanently, and this pass has been carrying them the
   whole time.** `rest=` — the rest-equality detector, which is supposed to read 0 in play — runs
   at tens of piece-reads per frame on a static base, from three to six units whose posed buffer
   is byte-equal to their own rest vertex arrays for the entire session with the pose flag set.
   Named by peeking `Object3do+0x0C → unit+0x92`: "Gaat Gun", "Sentinel", "Solar Collector" (both
   sides), "Vulcan", "Scorpion", "Wind Generator", and one aircraft, "Hurricane". Their `err` is
   therefore the model's own size (39–62) for as long as they are on screen, and `posefix` draws
   them from the reconstruction **every frame** rather than occasionally — which it does
   correctly, which is why nothing looked wrong. **Why the engine leaves those buffers at rest is
   not established.** It is a fact about the engine's buffer, not about our reconstruction, and
   G16 stops reading that buffer at all.

**Reproducing it costs scheduling pressure, not zoom.** The window is microseconds wide per unit
and opens ~30 times a second, so on an idle machine with cores to spare a walk of a minute usually samples it
never. Pinning the game to one core and putting spinners on that same core — its own render and
game threads then have to timeshare, which is what a loaded machine does to a player — brings it
to a handful of readings a minute. `_local` is not involved; a single core is used and nothing
else on the machine is touched.

**The regression, on the walk fixture** — Two Continents, one ARMCOM, static camera at
`eye (1818, 850)`, zoom 2×, three south legs over 62 s, the transient detector run over the world
band only (the frame's top carries the engine's message log, which scrolls on its own and
produced the largest transient of one capture without a unit ever being drawn wrong):

| capture | > 350 px | > 500 px | > 1000 px | worst |
|---|---|---|---|---|
| before the guard | 5 | **3** | **2** | 1403 px, the rest-pose draw |
| guard on, run 1 | 7 | 0 | 0 | 475 px |
| guard on, run 2 | 7 | 0 | 0 | 492 px |
| guard on, run 3 | 0 | 0 | 0 | 294 px |
| + rest-equality, run 1 | 0 | 0 | 0 | — |
| + rest-equality, run 2 | 0 | 0 | 0 | — |

⚠ **`[2026-09-09]` Those rows were driven by hand and their leg geometry was never written
down**, which is why re-running the protocol for G16's Gate C could not reproduce the `> 500`
column: the fixture built to replace them (`scenarios/walk-gatec.json` + `tools/gatec.sh`, at
1920×1080) walks a more energetic leg and reaches 520 px **on this same guarded path**. Treat the
`> 500` numbers above as a property of the original fixture, not as a threshold that transfers.
The `> 1000` band — the only one the rest-pose draw reaches — is the part that does transfer, and
it is 0 on every run of both fixtures. [GPU posing](gpu-posing.html) §4 step 7 carries the G16
table and the two caveats.

The `> 350` band does not move and is not meant to: it is the walk itself — a leg swing or a fast
yaw passes the "differs from both neighbours while the neighbours agree" test, and every ranked
frame up to ~600 px opened as ordinary animation. What goes to zero is the band only a wrong pose
reaches.

**Cost.** Per piece per unit per frame on the common path: two `int` reads and a compiler barrier
for the flag, plus — for the rest-equality test — a second stream of the piece's `nvert × 3`
words XOR-accumulated inside the copy loop that already runs, and one `IsBadReadPtr` on the node's
vertex array. That probe is the one avoidable part: the array is per model **type** and immutable,
so its readability could be resolved once per type the way `pmap_for` caches the piece map, and is
not. The reconstruction itself runs only on a trip. 60.0 fps before and after on the walk fixture,
which is one unit — **no measurement exists at 200 units**, for this or for any of the options
below. *[MEASURED 2026-09-09, once `tacli` stopped forcing the frame cap: on 200v200 at 1920×1080
with the sim paused, 281 units and 76 wrecks on screen, this path runs at **184.0 / 180.0 fps**
against the posed program's **313.0 / 306.9** — 1.70×. And it gets that while **truncating**:
`49152 verts VERTEX-BUDGET-HIT`, so it is drawing less than the scene asks for. §2.11 and
[GPU posing](gpu-posing.html) §4 step 7.]*

**A detector, not a lock — the residual window.** `Object3do+0x08` is a flag, not a sequence
number, so "zero on both sides of the read" means *no rewrite started and finished across the
read*, which is not the same as *no rewrite touched it*. A whole dirty-to-clean cycle falling
strictly between the two flag loads is missed. How much smaller that is than the bug it replaces
is the point: **before, any overlap at all between the pass's read and the rewrite produced the
artifact** — which is exactly what the captures show happening — **and now the rewrite has to be
strictly contained inside one piece's vertex copy.** From the trip rate (one in ~29 000
unit-frames on an idle reference setup, at the ~30 Hz the COB writes a piece) the dirty interval is around a
microsecond; one piece's copy is tens of nanoseconds. So a miss needs the *render* thread stalled
inside those tens of nanoseconds for at least the whole interval — and most of what that lets
through is benign anyway, two composed poses one tick apart mixed together. The rest-pose read
specifically needs the stall in the gap between a piece's last vertex load and the flag load,
with the entire remaining compose finishing in it.

**The second detector, which has no timing hole — rest-equality.** The reset copies `node+0x24`
over `prim+0x22` **verbatim** (`rep movs`), so mid-reset a piece *is* its node's vertex array;
composed, it is that array through the accumulated transform. A piece that compares byte-equal is
therefore either mid-reset or standing at an exactly identity transform — and the reconstruction
is right for both, because at identity it reproduces the array itself. This tests the data rather
than the clock, so no scheduling behaviour changes its answer. The compare folds into the copy
loop the pass already runs (one more load and an OR per component, both arrays streamed once).

It is applied only where equality would be a **contradiction** — the body turn, the piece's own
`TURN` or `MOVE`, or its rest offset from its parent is non-zero, so the accumulated transform
cannot be the identity. Without that gate a model facing exactly north whose base piece sits at
the origin would compare equal every frame and take the reconstruction forever: correct output,
for no reason. The gate reads local fields only, so it is conservative by construction — a piece
whose own fields are all zero under a rotated parent is skipped and left to the flag. It catches
what the flag can miss, never the reverse, and the two run together.

**Measured, and it is specific.** Under the amplifier with the fix off and the oracle armed, a
62-second walk on the reference setup tripped the guard 16 times; the **one** window whose oracle reading was a
mid-rewrite buffer (`err=22.23` model units, `dirty=1/1`) is the **only** window where the
rest-equality counter moved (`rest=1`). The other fifteen trips were stale buffers — validly
posed, one tick old, not byte-equal to rest — and it stayed at 0 on every one. In steady play it
does not fire at all: 19 of 20 five-second windows at `rest=0` across two shipping-configuration
runs, at facing 90 and at facing 0, standing and walking. The twentieth is the scenario load,
where `0x45AEC0` initialises `prim+0x22` as a copy of the node's array and it genuinely is
unposed until the first repose — the substitution is right there too. `rest=` rides the `native:`
line beside `guard=`, and it is the useful number of the two: it says how much of what the guard
caught was the dangerous kind.

**What is still not closed by either detector**: the "rotated but not yet translated" intermediate
(`0x45B150` rotates every vertex of a piece before adding the parent origin to any of them) is
neither byte-equal to rest nor flagged if both flag reads miss it — it is displaced by the parent
origin rather than collapsed, so it is a much smaller error, but it is not detected on the data
side. Closing everything outright still needs one of the two expensive options: a **sequence
counter**, which the engine does not keep and which we would have to synthesise with detours on
all three repose sites (two are inlined mid-function, at `0x45ACB6` and `0x45ADA5`, so byte
patches rather than prologue detours); or a **full content check** — compare against the
reconstruction on every frame instead of only on a trip, which has no timing hole anywhere but
pays a `pose_accum` per unit per frame. The architecture that removes the question entirely is
**G16** in the roadmap: pose 3DOs on the GPU from a static mesh, the way `tagpu_hires_draw.c`
already poses replacement models — that path never reads `prim+0x22` and the race cannot happen
to it. *[DONE 2026-09-09, step 8. Neither expensive option was ever built, and neither is needed:
the question was removed rather than answered. What is left is a strictly smaller residual — a
pose mixed across one tick — named at the end of §2.11.]*

**Not closed by this.** The same live read is made by `tagpu_hires_draw`'s replacement-mesh path
through `hires_pose`, which reads the pose *fields* rather than the buffer and so cannot show the
rest pose — but it can show a pose mixed across two ticks, which nothing here measures. And the
guard says nothing about the *anchor*: the unit's 16.16 position is read without any interlock,
which is sound for a single aligned dword but has never been checked across the three of them.
*[2026-09-09: since step 8 EVERY unit is drawn the way this paragraph describes the hires path,
so what was a note about one pass is now the whole renderer's residual. Both halves of it are
still open and still unmeasured, and §2.11 carries them.]*

### 2.10 The per-type geometry bake (`tagpu_posebake.c`, OFF by default, `tagpu_posebake.on`) — G16 step 4

**It draws nothing.** This is the data half of [GPU posing](gpu-posing.html) — the step that turns
a `Model3DONode` template into the two static vertex buffers a posed shader will draw a unit from,
so that the emitters above stop reading `prim+0x22` and §2.9's whole apparatus can be deleted. The
shader is step 5; until then the module bakes, caches, invalidates and **checks itself**, and with
the trigger absent it costs one `GetFileAttributesA` every 30 frames and the `native:` line is
byte-identical to what it was.

| | |
|---|---|
| **the geometry buffer**, per type | rest position, the rest normal of the vertex's own triangle, the piece index and a flags word — 8 floats — with body triangles, slant triangles and wire lines laid down as three ranges of one buffer. Keyed on primitive 0's node pointer plus the level and GL generations |
| **the material stream**, per (type, owner, atlas generation) | UV, flat colour, colour key and a **skip** flag — 5 floats. The two buffers always hold the **same** vertex count: a face the engine paints nothing for is baked and collapsed by its flag rather than dropped, which is what lets either be rebuilt without the other |
| **the topology**, per type | the parent array and the accumulated rest offsets, so `pose_accum_body`'s per-unit sibling scan stops being a per-frame cost. Cached now, consumed in step 5 |
| **invalidated by** | the level generation (the frame packet's `level_gen`), the GL generation, the **atlas** generation — new, `TAGPU_GAFATLAS.gen`, bumped by every `atlas_reset` and `atlas_lost` because every UV moves — and the owner, which is in the key |

**Fields we write: none.** Every engine read is the one the emitters already make.

**What it measured on the pose inventory** (67 distinct types, four camera stops, `posebake.on=log
check`): **0 check failures** on either of the two things the lever compares — the body vertex
count `emit_geom` produced against the bake's own prediction, and the accumulated rest offsets
against `pose_accum_body`'s per-unit walk — with `anom=0` and `refused=0`. The two ordinary
findings (`odd=` faces outside `3 ≤ fvc ≤ 32`, `nomat=` faces with neither texture nor colour) are
content, not faults; [GPU posing §3](gpu-posing.html) carries the correction that says so. The GL
invalidation was watched on an exit to the shell: `posebake: dropped 53 geometry (taking 53
material with them) … GL 2`. *[The 27 this first quoted was an earlier run of the same test, before
the drop line reported the cascade separately; both are real, but only one is the shipped build.]*

### 2.11 The posed program (`tagpu_posedraw.c`) — THE unit renderer, G16 steps 5-8

**There is no lever and no alternative.** `tagpu_posedraw.on` was a measurement lever while the CPU
emitters were Gate B's oracle; step 8 deleted them, so this pass draws every unit or the unit is
not drawn. A unit is one `glDrawArrays` out of its type's geometry and material VBOs with its whole
pose in a uniform block; no vertices are built for it on the CPU at all. That covers **all three
baked ranges** — the body, the structure-shadow slant and the nanoframe wireframe — which was every
reader of `prim+0x22` except the selection lines and the effects models, and those two are all that
is left on the shared stream.

| | |
|---|---|
| **the program** | a TWIN of the native one: its own vertex stage (the port of `emit_node` — piece transform, `sx = ax + x` / `sy = ay + (-z - y/2)`, the depth key, the world x/z the fog samples, the model height the waterline clips on, and the shade quantised off the baked rest normal), and the native pass's **own** fragment stage, taken through `tagpu_native_unit_fs()` rather than copied |
| **the shadow-depth twin** | the same vertex shader with an empty fragment shader and `uDepthPass = 1`, mirroring `tagpu_shadow.c`'s `VS_U`/`FS_NONE`, so a colour-keyed texel casts on both paths |
| **the Classic silhouette** | routed through the posed program too — it reuses the body geometry, so a posed unit would otherwise lose its shadow whenever Classic++ is off |
| **who gates it** | `shadows=`, not the Classic++ master arm and not `assets`/`light` (G18a/G18b, merged in 2026-09-09). Both posed shadow loops in `tagpu_native.c` skip on `cpp && !hard`, so **Classic++ at `shadows=2` (HARD) draws the Classic pair through this program** and `tagpu_shadow_begin` then refuses the depth pass outright (it requires `TAGPU_SHADOWS_SOFT`, `tagpu_shadow.c:368`). At `shadows=0` nothing is drawn, an aircraft's `airshadow=drop` included. This gating was written against the CPU emitters G16 step 8 deleted and was **re-expressed**, not merged |
| **the ground does NOT cast** (2026-09-09) | `terrainshadow` now defaults to **0** (`tagpu_classicpp.c` `shadow_defaults`), so `tagpu_shadow.c:398` returns before `tagpu_terr_hills_draw` and the heightfield contributes nothing to the depth map. Units are unaffected — this is the caster's own gate, not the shared bias. The ground casting on itself darkened open water in the caster's own 16-unit lattice and got worse as `Shadow quality` went up; seven candidate fixes were swept and costed and every one removed the artifact and the terrain-shadow feature together, because a cell's own relief *is* the terrain shadow ([renderers](renderers.html) §2.7b). `terrainshadow=1` in the cfg still turns it on and is the fixture the eventual fix is measured in; the render-options screen has no row for the key and never writes it, so nothing a player can click re-enables it. The hills mesh is still BUILT at map load (6.4 MB + 12.9 MB on Two Continents) — it is simply never drawn, which is an **open TODO**: `build_hills` is unconditional where the draw is gated, and the fix is a lazy build on first draw rather than gating the build (the flag is live, and `terrainshadow=1` mid-session is the fixture the shadow fix is measured in). See [roadmap](roadmap.html) |
| **the three ranges** (step 6) | `uRange` selects. **BODY** as above. **SLANT** takes `0x45A610`'s projection `(x + y/4, −z − y/4)` off the posed vertex snapped to whole units, the neutral SHD row, `waterT`/`digT` pinned at −1e9 by the pass itself (the structure branch never erases — the G14j fix), and its own per-piece rule `(P_FLAGS & 3) == 3`. **WIRE** is `GL_LINES` on the body projection, one notch nearer (+0.15), the nanoframe's animated blue from a uniform because it is per unit while the material stream is per type and owner |
| **the 16.16 snap** (step 6) | the posed vertex is rounded onto the engine's own grid — `floor(m·65536 + 0.5)/65536` — **before anything reads it**, in every range. The engine holds each posed vertex as three 16.16 integers and every CPU emitter reads them back as `v[i]/65536.0f`, so a float compose that stops short sits up to half an LSB off a value that is exactly representable; `recon_prim` rounds the same way. This is what makes the slant portable at all (its `>>16` is a FLOOR, so half an LSB is a whole screen unit) and it took the body's residual to zero as well |
| **the pose** | a std140 block, `vec4 uRow[3*256]` + two packed per-piece words, `uPieceFlag[64]` (shaded) and `uPieceVis[64]` (0 not drawn / 1 drawn / 3 drawn and casting) = **14 336 bytes**. `GL_MAX_UNIFORM_BLOCK_SIZE` is read at build time and the pass **refuses to arm** below that. Since step 8 there is no CPU emitter to leave those units to, so the refusal is *published* and `owndraw` stops skipping the engine's own unit rasterise — see "when it cannot arm" below. The wire reads the same word rather than the all-zero matrix: a zero-area triangle provably produces no fragments, a zero-length LINE is not promised away, and one bright pixel per hidden edge would land on the unit's origin |
| **the topology** | consumed from the bake entry's `parent[]` — `pose_accum_body`'s per-unit sibling scan is gone from the posed path, which is what §2.10 cached it for |
| **the VAO** | one per material stream, built at bake time, binding the geometry buffer (locations 0-3) and the material stream (4-6) together, so a draw is one bind |
| **the model top** | `s_emitTop` is gone; the top no longer falls out of the vertices: it is each piece's baked BODY-range rest AABB through its pose matrix. An **over-estimate** (an AABB through a rotation bounds the posed points), and it covers faces the material stream collapses. Wrecks only — a unit with a record prefers `model_aabb` |

**Fields we write: none.** Every engine read is one the emitters used to make; the pass adds GL
objects and no engine state.

**The three degradations, and why none of them is a fallback** (step 8; the full ledger with its
invariants is [GPU posing](gpu-posing.html) §4). There is one renderer, so every condition that
used to fall through to `emit_geom` now degrades *inside* the unit. All three ride the `native:`
line beside `posed=` and are printed **only when they have caught something**, because in a healthy
game none of them ever does:

| field | what happened | what the player sees |
|---|---|---|
| `rest=` | the frame's pose arena was full | that unit drawn **at rest** for the frame — right geometry, material, position, fog, shadow and depth, only its animation frozen. The block it takes is one shared static set of identities, so the degradation allocates nothing and cannot itself fail |
| `unpl=` | a piece's node did not read, or its parent link never resolved | that **piece** at rest inside a unit that is otherwise posed — `hires_pose`'s answer for the same condition |
| `nobake=` | the type would not bake: over `TAGPU_PBMAXPIECE` (256) pieces or past `PB_MAXVERT` (49152) vertices | **nothing drawn for that unit.** The one honest drop, and it is counted whether or not `posebake.on` is armed — without a count an undrawable model is a unit missing from the screen with nothing in the log. Stock's worst model is 36 pieces and 574 vertices |

`q=` is the fourth and is not a degradation: it prints the units the gather **queued** against the
units the pass **drew**, only when they disagree, because since step 8 a queued unit the draw
dropped is a unit missing from the screen. It came out of chasing a `posed=` that read lower than
the unit count, which turned out to be the **hires** pass taking those units — not a miss.

⚠ **The arena degradation looks exactly like the artifact §2.9 exists to describe** — a unit at its
unrotated rest orientation. That is not a coincidence and it is the reason it is counted: it is the
same picture, but bounded, deliberate and visible in the log, rather than a race. Forced with a
48-slot arena it reads `rest=6900` with the **same unit and triangle counts** as the healthy build:
every unit still drawn, nothing dropped.

**When it cannot arm at all** (step 8, decision B). `owndraw`'s detours skip the engine's own unit
rasterisers and are installed at DLL attach, so with no CPU emitter left "draw nothing" would be
the default failure — every unit in the game invisible. So `tagpu_owndraw_classify` **asks before
it skips**: it reads a readiness word this pass publishes, and hands the draw back to the engine
(8bpp, composited through the terrain key) when the pass is not live. That read crosses threads and
is safe by **direction**, not by timing: the word is one aligned `int`, written only by the render
thread, set to "live" only after the programs have linked, and cleared by `tagpu_posedraw_glreset()`
*before* a new context is used. A stale "not ready" costs a one-frame double draw; a stale "ready"
is the unsafe direction and no write order produces it. The log line fires only once the pass has
**tried and failed**, not during the ordinary first frames before the render thread has built
anything.

**What `MAXNV` stopping applying to units actually looks like.** The claim used to be a design
note; `scenarios/crowd-static.json` (256 units of 16 types, one owner, no orders — the large scene
that stays identical between runs, so unlike a battle it can be diffed) makes it a picture. On it
the CPU emitters log `49152 verts VERTEX-BUDGET-HIT` and **the bottom rows of the block are health
bars with no models**: the shared stream ran out mid-gather and the units after it got empty
ranges. The posed path draws all 256 — `posed=240/32288tri slant=96/13120tri`, plus 16 hires
ARMPWs, and 96 slant is the 6 structure types x 16 — because per-type GL buffers have no shared
budget to exhaust. It wanted ~136k vertices where the stream holds 49 152.

**The frame time, at last** *[MEASURED 2026-09-09; it needed `tacli --maxfps 0`, since both paths
had been reading 58.5 fps because both hit the cap]*. 200v200 at 1920x1080, sim paused, 281 units
and 76 wrecks on screen: the CPU emitters **184.0 / 180.0 fps** against this pass's **313.0 /
306.9** — **1.70x**, and the CPU side gets that while truncating.

**The residual this pass leaves.** It reads the pose fields on the render thread with no interlock,
so a unit can be drawn with its pieces mixed across one tick boundary — strictly smaller than what
§2.9 describes (there, the engine's buffer lagged the fields by a whole tick for the entire unit),
and it cannot produce the rest pose, because nothing it reads is ever `rep movs`'d from a rest
array. Gate C looked for it over 62 s under scheduling pressure and found nothing visible
([GPU posing](gpu-posing.html) §4 step 7). The anchor is the other half and is still unchecked: the
unit's 16.16 position is three dwords read without an interlock.

**What it measured**, 1024x768, `ss=2`, the sim paused, one build, twelve scenes —
[GPU posing](gpu-posing.html) §4 step 6 has the full table. With `poserecon.on` on **both** sides,
which is Gate B's protocol and isolates the port: **0 differing pixels of 786 432 on eleven of
twelve scenes and 1 on the twelfth**, across four camera stops of `pose-inventory`, `200v200` at
128 posed units and 13 219 triangles, a six-nanoframe wire sweep, and all five G14j shadow
fixtures. Against the engine's own posed buffer the worst scene is 43 pixels (`shadow-struct`) —
but **that column is the reconstruction, not the port**: running the CPU emitter against itself
(`poserecon.on` vs off, the posed pass out of the picture) reproduces it row for row, and it is in
the shipping build today because the guard falls back to the reconstruction on a torn read. Every
differing pixel in the table is isolated; nothing is the "whole face one SHD row off" tell that
would mean the shade quantisation is wrong. **Gate B and Gate D both PASSED.**

*[The step-5 numbers this paragraph used to carry — 2 pixels and 1 pixel — were the shader not
snapping the posed vertex onto the engine's 16.16 grid. Step 6 does, for every range, and the same
fixtures read 0.]*

**A trap this cost a crash to find.** `opengl_utils`' global `glGetIntegerv` is **NULL**:
`wglGetProcAddress` returns NULL for GL 1.1 core entry points under wine, which is why
`opengl_utils` guards its own use of it and why `tagpu_terr.c`, `tagpu_shadow.c` and
`tagpu_restoreglsl.c` each load their own through a `getgl()` that falls back to `opengl32.dll`.
Calling the global one is a jump to address 0 — `ErrorLog.txt` reads `Access Violation … at
0023:00000000`, during map load, with nothing in `tagpu.log` because the module dies before its
first line.

### 2.12 The render-options screen (`tagpu_menu.c`, `tagpu_ufo.c`, on by default, `tagpu_menu.off`) — Phase F G18

The in-game settings screen. Design and the decisions behind it:
[renderers](renderers.html) §2.10; the engine reading: [engine map](exe-reverse-engineering.html)
*The screen lifecycle*, *A screen's own GAF*, *Setting a gadget's state*, *Where the engine
looks for archives*. It is a **real TA `.GUI` screen**, so the engine does the hit-testing,
the dispatch, the plate art, the fonts and the save-under, and the G15/G17 twins carry it for
free; what we supply is two bytes of gadget state, one frame's pixels, one 28×28 trigger and
one small archive.

| site | what we do there | thread |
|---|---|---|
| `DLL_PROCESS_ATTACH` | write `impure-patch.ufo` (`guis/render.gui` + `anims/render.gaf`) unconditionally, with a version stamp. `DDRAW.dll` is TotalA.exe's first static import, so this precedes `InitTAHPIAry 0x41D4C0`'s `*.UFO` glob by the loader's rules | — |
| `DrawGameScreen 0x468CF0` (observer) | the per-frame tick: sample the trigger file on its edges, open or close, re-assert `main+0x37EA0`, and recover if the screen was freed under us | game |
| `0x46A308` (observer, post-GUI) | blit the sprocket into the back buffer with `CopyGafToContext 0x4B7F90(NULL, frame, x, y)` | game |
| `GUIMEMSTRUCT+0x08` (our `OnCommand`) | advance the row, `GUIGADGET_SetStatus 0x4A1080` for every row, `grayedout` for Shadow quality, set the repaint flag `gi+0xCCA` via `0x49FA90`, and **answer the pump with `0x4AB0A0(gi)`** (`gi->UIChange_f = -1`). **It writes no file** | game |
| `tagpu_shield.c`, both paths | `tagpu_menu_click()` — the sprocket's hit test, from `deliver_mouse()` (injected) and from the wndproc before the shield's gate (real) | game |
| `tagpu_zoom.c`, two entry points | `tagpu_menu_owns_point()` — the zoom transform must leave a point the menu owns alone. The panel hangs over the world and the transform's gate is geometric, so at any zoom ≠ 1 a row click was bent away and **no row worked**; `tagpu_zoom_drop_mouse` must not treat it as the display-only ring either | game |
| `render_ogl.c`'s frame | `tagpu_menu_present()` — the deferred cfg/lever write | render |
| open | `GUI_Load 0x4AA8F0(gi, main+0x37EA0, flags)` with `0x20` + `0x400`, patch the panel rect, set `+0x08`/`+0x0C`, then `0x4C2470(); GUI_StageUpdateDraw(gi, 0x21); 0x4C2870()` — GUI_Load's own suppressed stage 1, reproduced; then repaint the ground and set `gi+0xCCA` | game |
| close | restore `main+0x37EA0` and call `UpdateIngameGUI 0x491D70(1)`. **`GUI_Pop` is never called** | game |

**Answering the pump is not optional** [VERIFIED 2026-09-11]. `gi->UIChange_f` is
bidirectional: the pump writes the actuated index into it before calling `OnCommand`
(`0x4AA675`) and reads it back afterwards (`0x4AA79A`), and a handler that returns with it
still set is asking to be popped — whereupon `0x4AA7BC..0x4AA7FA`, an **inlined copy of
`GUI_Pop`'s body**, relinks the stack and `free()`s the `GUIMEMSTRUCT`. Every engine handler
ends with `0x4AB0A0(gi)` for exactly this reason; see *The pump's dispatch contract* in the
[engine map](exe-reverse-engineering.html).

**The front-end screen carries fifteen rows in two columns** [2026-09-11]. `VISUALS.GUI`
is re-emitted into the same `.ufo` with the stock eleven gadgets moved (names, `assoc`,
`commonattribs`, `range` and `stages` all verbatim) and four rows of our own added:

| column | rows |
|---|---|
| **Window** | Display mode (window / borderless fullscreen, `util_toggle_fullscreen`), Monitor (`EnumDisplayMonitors`, `SetWindowPos`), UI scale (Auto / 1x..4x, the client set to k x the Screen Size row's own mode at `main+0x37F1B/+0x37F1F`), Screen Size (stock `VIDSLDR`), Frame cap (60 / 120 / uncapped, `g_config.maxfps` + `fpsl_init`), Gamma (stock) |
| **Impure rendering** | Renderer, Undithered assets, Dynamic lighting, Shadows, Shadow quality, Shading (stock), Anti-aliasing (stock), Engine shadows (stock `BSHADOWS`), Supersampling |

Three things this rests on, each measured rather than assumed:

- **The four Window rows are applied on the thread that owns the window.** Each ends in a
  window call, and a cross-thread one is a wait on a message pump rather than a visible
  error, so the click POSTS `WM_TAGPU_DISPLAY` and the wndproc does the work — the same
  contract `tagpu_shield.c` uses for injected input.
- **`util_toggle_fullscreen` does not restore the window size on the way back** (measured:
  1024x768 -> 3840x2160 -> 3840x2160), so the Display mode row saves the windowed client
  before leaving and puts it back itself.
- **The monitor is a rect, never a display "mode".** On a multi-monitor X server wine
  reports the VIRTUAL DESKTOP as the current, registry and largest-enumerated mode, for
  every adapter alike — 6200x2160 on the reference setup's three outputs. So the Monitor
  row, the borderless-fullscreen size AND position, and the Screen Size list all come from
  `util_target_monitor` (`utils.c`), which asks `GetMonitorInfo`. The measurement, and the
  per-monitor mode lists it produces, are in [resolution](resolution.html) §6.6.
- **The ground is ours.** STARTOPT's background paints one column of recess bars across
  x 267..405, drawn for a single centred column; two columns cannot sit in it and it is
  the game's art. So the screen carries one `id=12` ground frame (270x420 at (200,54)) in
  `anims/visuals.gaf`, with its own recesses, covering those bars.

**The Monitor row rebuilds the screen**, because the Screen Size list belongs to a monitor
and the engine builds it once per visit (`0x45E6B0` into `GUIMEMSTRUCT+0x0C`, hung off
`VIDSLDR` at `0x45E726`). There is no "re-enumerate in place" call, so the row uses the
engine's own idiom for a stale screen — `GUI_Pop(gi)` then `0x45E5E0(0)`, verbatim what
UNDO does at `0x45E31E` — with `s_visKeep` set so the rebuild keeps the model. `GUI_Pop`
writes -1 into `gi->UIChange_f` (`0x4A9673`), so the pump is answered and there is no
`menu_accept` to do. **The list follows the model, not the window**: the move behind that
row is posted, so at rebuild time the window is still on the old monitor — measured
2026-09-11, the three lists come out 8 / 3 / 3 entries as the row is cycled, and the window
follows one message later. In fullscreen the row applies as a single
`dd_SetDisplayMode(0, 0, 0, 0)`, which re-derives position, size and render target from the
same monitor; placing the window here as well left it a pixel taller than the screen and
back at the primary's origin, because the re-apply places it last.

**Restore Default and Undo Changes reach our rows too.** Both are STARTOPT's buttons and
both end in `GUI_Pop` + `0x45E5E0(0)`; we handle them before forwarding and set `s_visKeep`
so the rebuild seeds the plates from the model rather than re-reading levers the deferred
write has not reached yet. **Undo** restores every row the screen opened with, Display mode
and Monitor included — it is the escape hatch for a mode the player cannot see the menu on.
**Restore** sets the rendering rows to the Classic++ defaults, UI scale to Auto and the cap
to 60, and deliberately leaves Display mode and Monitor alone: a default that moves the
window to a monitor the player cannot see would hide the button that undoes it.

Until 2026-09-11 this screen did not, so **every click popped and freed it**, and the tick's
`on_stack` recovery re-opened it with `fresh == 0` the same frame — which is why it looked
like it worked. On the front-end screen, where there is no recovery, the same omission simply
closed the screen; the `GUI_Pop 0x4A9660` observer that was armed to catch it could never fire,
because the pop is inlined. The recovery path stays (the engine really does tear the in-game
GUI stack down on a world click) but an ordinary click no longer reaches it.

**Fields we write.** `main+0x37EA0`, the expected-screen name buffer — 16 bytes, saved and
restored, and re-asserted every frame while the menu is open. Gadget `status_curnt` (`+0x137`)
through the engine's own setter, and `grayedout` (`+0x13C`) by direct field write, which is
the engine's own setter `GUIGADGET_SetGrayed 0x4A1250(gi, name, grayed)` — **not** a direct
field write. (An earlier revision stored a 32-bit 0/1 into `+0x13C` and cited `0x477416` as the
precedent. Both were wrong: `0x477416`'s writes are `mov BYTE [esi+0x137],0/1` at `0x47743B`
and `0x47746A` — `status_curnt`, a different field — and `+0x13C` is a **u16 whose bit 0 is the
flag**, which the engine read-modify-writes so bits 1..15 survive.) The panel record's
`xpos`/`ypos` (`+0x13`/`+0x15`),
because the panel is right-aligned and a `.GUI` written at attach cannot know the resolution.
`gi+0xCCA`, the repaint flag. And the loaded GAF frame's colour plane (`+0x10 PtrFrameBits`),
repainted in place with the composed ground. **Nothing sim-side, and nothing that replicates.**

**Seven rows since 2026-09-09**, the seventh being the FPS readout (§2.14). Two things about it
are unlike the other six, and `read_state()` and the click handler have to agree on both or the
row would read one way and behave the other:

- **It never makes Renderer read `Custom`.** Every other row below Renderer changes what the game
  *looks like*, and `Custom` is the honest answer for those; this one draws a diagnostic over the
  finished frame and changes no pixel the game rendered. So the click handler skips the
  `STYLE_CUSTOM` assignment for it and `read_state`'s `custom` test leaves it out.
- **It is never greyed.** Like supersampling, it is orthogonal to the Classic/Classic++ lane, so it
  is not one of the switch's dependants — grey it with the lane and a player on Classic could not
  turn it on.

`PANEL_H` went 212 → 240 for the seventh row: `ROW_Y0 34 + ROW_PITCH 28 × 6 = 202` and `ROW_H` is
20, so the last row ends at 222 and 240 leaves the 18 px the six-row panel left below 194. One
define carries it — the GAF frame header, the ground, the ramp, the border and the corner bolts
are all sized from it — and the panel is **composed at runtime** from the player's install rather
than shipped, so no art is regenerated. `tools/guipanel.py` still says 304×212: it is the lab's
copy of this layout, not its source.

**Files.** `impure-patch.ufo` every launch; `tagpu_classicpp.cfg` rewritten preserving every
key the screen does not own (`sun`, `amb`, `penumbra`, `shadowlen`, …); `tagpu_ss.off`
created and deleted; `tagpu_fps.on` created and deleted (§2.14); and **both**
`tagpu_classicpp.on` and `.off` — the pair, because
`tagpu_opt.c`'s precedence is an `.on` wins and an `.off` only defeats a *default*-on, so
driving one of them applies nothing on an instance that carries `tagpu_defaults.off` or a
hand-armed `.on` (§2.8). Write access to the gamedir is not new — the DLL
already writes seven files from seventeen create-for-write sites.

**The trigger is not a gadget and cannot be one.** No GUI screen owns the top bar (every
in-game panel is the side panel at `(0,128) 128×352`) and a gadget is drawn into its panel's
own `w×h` surface at panel-relative coordinates, so a gadget at `x=1876` has nowhere to be
drawn. It is 28×28 at `(w−16−28, 2)`, sharing `MARGIN = 16` with the panel at `(w−16−304, 32)`
so the two right edges land on one line — measured at both 1024 (988, 704) and 1920 (1876,
1600).

**Known cost.** The engine rebuilds the whole in-game GUI stack on a world click, and the
panel hangs over the world, so clicking a row costs one frame of the panel being re-pushed.
The row's own state survives it (the model is ours; the levers are read only on the player's
open), but the rebuild is visible if you look for it.

### 2.13 Sub-tick pose interpolation (`tagpu_lerp.c`, OFF by default, `tagpu_lerp.on`) — 2026-09-09

The design, the invariants and the gate results are [smooth motion](smooth-motion.html); this is
the hook-map entry. **No engine address, no patch, no engine write.**

`posed_pose` composes each piece's 4×3 from exactly `pr+P_POS` (i32[3] 16.16) and `pr+P_TURN`
(u16[3] TAang). With the lever armed, those two reads are served from a blend of the unit's last
two **sim-tick** snapshots instead of the live fields, so the model animates at render rate rather
than snapping at the tick. It is only this small because G16 turned the pose back into per-piece
*fields*; before it, the pose arrived already baked into vertices.

- **Read-only.** The history arena is ours (§2.5). Nothing is written back to `PrimitiveStruct` —
  the sim reads those fields and TA has no runtime desync detection, so a per-machine
  framerate-dependent blend written there would diverge two peers silently.
- **The degradation is a refusal, not a weight.** No history, a stale pair, a model over 48 pieces,
  a full table, or a weight that has reached 1.0 — every one returns 0 and the caller reads the
  live fields with its own untouched code. Not "blend at weight 1.0": `a + (b - a) * 1.0f` is not
  `b` in floating point, and the parity claim would have been false.
- **The tick period is measured, not assumed.** `main+0x38A47` advances `3 × GameSpeed` a second
  and a skirmish starts at `GameSpeed` **20**, i.e. 60 ticks a second — [engine
  map](exe-reverse-engineering.html) §"The simulation clock". The module learns the period from the
  observed interval divided by the tick gap, so it is right at any speed and follows a live change
  (`p=33.2ms` at GameSpeed 10 against a predicted 33.3). **On pause it degrades by the same
  refusal**: no new tick arrives, the weight passes 1.0, and every unit draws from the live fields.
- **Not smoothed, deliberately**: `pose_accum_body` — the path `hires_pose` and `tagpu_posedump.on`
  take — is untouched, so replacement meshes still step and the sim oracle still reports the
  engine's own fields. The unit's world position and `O3_BTURN` are out of scope, which means the
  pose runs a sample behind a position that is not delayed.
- **Cost**: the first cut's `double` blend measured `+0.560 ms a frame at 240 posed units` — 2.33 µs
  a unit, 3.4 % of a 60 fps budget, on the paired `crowd-static` fixture
  ([smooth motion](smooth-motion.html) §7f). **The shipped blend is 16.16 fixed point and that
  number no longer describes it**: the loop now carries no x87 at all, where the float form carried
  13 instructions including 4 `fldcw` (§7i, established from the compiler). Its frame cost is
  **not re-measured** — an interleaved live A/B put every off/on pair below 0.560 ms but spread
  0.068–0.461 ms under load from three other instances, which is not a number. §7i says how to
  close it.
- **The bound on the weight is clamped, not argued.** `w16` is forced into `[0, 65535]` because the
  turn multiply has only 32767 of headroom (`t` reaches +32768; `32768 × 65535` is 2 147 450 880
  against `INT_MAX` 2 147 483 647). The `[0,1)` refusal above already implies it today; the clamp is
  what keeps a later change to that refusal from becoming silent signed overflow.
- **Known residual, open**: the `(o3, nparts, gen)` key does not separate two units of the **same
  type** landing on the same reused `Object3do`, so a freshly built unit drawn within `LERP_MAXGAP`
  of a dead one's last sample can blend one frame from the dead unit's stance. Cosmetic and bounded
  — every index stays inside the same 48-piece block — but not closed; closing it wants a stable
  per-unit identity (`unit+0xA8`) rather than the allocation address. Found by the landing review.
- `lerp=<blended>/<snapped> p=<ms> u=<weight>` rides the `native:` line, and **nothing at all** is
  printed when the lever is off.

### 2.14 The frame-rate readout (`tagpu_fps.c`, OFF by default, `tagpu_fps.on`) — 2026-09-09

A row on the render-options screen (Off|On) and the module that draws it. **No engine address, no
patch, no engine read at all** — it counts our own presents and draws over the finished frame.

| site | what we do there | thread |
|---|---|---|
| `tagpu_overlay_draw`, after `tagpu_gui_present` | `tagpu_fps_present()` — the readout, drawn **above** the UI layer so the side panel and dialogs cannot hide it | render |
| `tagpu_overlay_draw`, the context-change branch | `tagpu_fps_glreset()` alongside the other modules' | render |
| `tagpu_menu.c` `write_levers()` | create or delete `tagpu_fps.on` — the deferred write, off the game thread, exactly as `tagpu_ss.off` is written | render |

**Files.** `tagpu_fps.on`, positive sense (present = on), polled every 30 frames on the render
thread. The row and the file are one setting, so either can drive it.

**Why not the one that already exists.** cnc-ddraw's own `dbg_draw_frame_info_start` sits behind
`tagpu_fpsosd.on`, but it is compiled only under `_DEBUG` and GDI-draws into the engine's 8bpp
surface, where it beats against the engine's redraw of that area and flickers — its own comment
says so. A `_DEBUG` build would also move the very numbers a frame-rate readout exists to report.

**The atlas is a cache, and that decides the design.** `tagpu_text_place` keys on the WHOLE STRING
and packs it onto a shelf that is never freed until the font changes, in a **fixed** 512×256 atlas
(`tagpu_text.c`). A readout that placed `"FPS 101"`, then `"FPS 102"`, would burn one permanent
entry per distinct number, exhaust the table within seconds and then start dropping — and it shares
that atlas with `tagpu_mark`'s group digits and the `ShowRanges` labels, so it would starve those
too. This places **eleven fixed strings** — `"FPS"` and `"0"`..`"9"` — and emits one quad per
character. Eleven entries, once, for the life of the font.

**Screen space, not world space.** The marker pass's text is world-anchored: its quads carry
`(wx, wz)` for the fog lookup and take the zoom transform, so a readout drawn through it would
fade into fog and scale with the camera. This has its own two-triangle program in game-frame
pixels and nothing else.

**Threads.** Render thread only. It reads the font the game thread published (`tagpu_text_snapshot`
at hook 8) through `tagpu_text_frame`'s per-frame latch, exactly as the marker gather does.

**Verified in the game 2026-09-09** on `crowd-static` at 1024×768: `fps: armed …` in the log and
the readout drawn and legible. *Two things to know before reading a capture of it.* It is drawn at
`(6, 6)` in game-frame pixels, which in a normal layout is **over the minimap** — deliberate for a
diagnostic that is off by default, but it means a `glshot` of that corner shows the minimap's own
grey/yellow view rectangle behind the digits, which reads convincingly as a corrupted glyph until
you take the pair with the readout off. And the glyph advance is `w + 1`, so it renders as
`FPS175` with no gap after the label.

### 2.15 HUD scale (`tagpu_hud.c`, **off unless armed**, `tagpu_hud.on`) — Phase F G18f

The in-game HUD magnified inside the player's own Screen Size, over a world the engine goes on
drawing exactly as it always did — so Screen Size and HUD size are two dials rather than two
names for one number. Design and every decision behind it: [GUI renderer](gui-renderer.html)
§22, and **§22.5 for why the first build was withdrawn the same day**; the geometry it rests
on: [resolution](resolution.html) §3.4a; the engine reading: [engine
map](exe-reverse-engineering.html) *The viewport rect at game entry* and *The world→screen
projection is NOT derived from the viewport rect*.

**It writes no engine memory at all**, and that is the whole of the correction. The first build
reserved the space by writing the viewport rect; `L`/`T` are the screen→world origin inside
`0x498DA0` and nothing else, while TA's world→screen projection is a `+0x80`/`+0x20` pair of
baked immediates, so the world tore in two by `((s−1)·128, (s−1)·32)` — measured, 1024×768
Auto: the engine picked a unit 76 px left and 19 px up of where it was drawn. The HUD now
covers the outer world instead of asking for it.

| site | what we do there | thread |
|---|---|---|
| `LAY_FS` (`tagpu_gui_surf.c`) | `uHud` — the two integers, `1/s` and `s`. Three regions sample the twin at `s` texels per device pixel; the ramp widens by `s` with them. Inert at `s = 1` | render |
| `mouse.c`, `winapi_hooks.c` ×4, `wndproc.c` ×3 | `tagpu_hud_to_engine()` at the end of every client → game conversion: inside a HUD region the engine is handed the point on its own 1× HUD grid | message |
| `sharp_cursor`, `sharp_minimap` | `tagpu_hud_to_screen()` — the engine's own cursor position (the fallback path only) and the minimap's box go the other way, so the sharp layer lands on the magnified art | render |
| `tagpu_menu.c` | the "UI scale" row, `trigger_rect()` and `panel_rect()` | game / window |
| `0x4288D0` (observer), gated on return address `0x498242` | write `R`, `B`, `viewW`, `viewH` so the engine's viewport IS the visible window — `L`/`T` untouched, because the projection bakes them. Writes nothing at stock | game |
| `tagpu_native.c`, the world composite's `glViewport` | the one draw that puts the world target on the frame, shifted by `(128s−128, 32s−32)` | render |
**One resolver, so the two halves cannot disagree.** `tagpu_hud_geom(W, H, pct, …)` is a pure
function that clamps to the screen's own ceiling and yields `s`, the panel width and the bar
height. The composite and the pointer map both call it; neither owns the answer, and because
`128s / s` is exactly `128`, the screen column where the panel ends is exactly the engine
column where the world begins. The ceiling is `H/480` — [resolution](resolution.html) §3.4a
measured the panel to be a fixed 128×480 block that does not stretch — with a second bound
that keeps at least 256 px of world width.

**Fields we write.** Four of the six viewport ints at `main+0x37E2F..0x37E3B` (`R`, `B`, `viewW`,
`viewH`), once per game entry and only when the resolved scale is past stock. **`L` and `T` are
never written** — that is the whole lesson of §22.5. Because viewW/viewH size the SORT buffers at
LoadMap, the setting is game-entry-time: a scale chosen mid-game waits for the next game.

**Why a stale scale is safe.** One word crosses threads — the percentage in force — and every
consumer re-resolves it against the screen *it* sees, so either value a racing 32-bit read can
return is one that fits that screen. The worst a mid-frame change can do is leave the pointer
map and the picture one frame apart, and neither is engine state. The shell needs no signal of
its own: its surface is atom-locked at 640×480 whatever Screen Size says, and 640×480's ceiling
is exactly 1.0.

**Files.** `tagpu_hud.on` (`scale=auto` / `scale=<percent>`) and `tagpu_hud.off`, written as a
pair for the reason §2.8 gives.

**Known costs.** The HUD is 1× art magnified: bigger, not sharper. That is now the only one:
§22.6 made the engine's viewport the visible window, so the whole map is reachable, the camera
centres on what the player sees, and the world under the HUD is no longer drawn at all.
### 2.16 The frame packet exchange, landing 1 (`tagpu_packet.c`, `tagpu_packet_pub.c`, on by default, `tagpu_packet.off`) — 2026-09-12

The first step of the plan the cross-thread audit led to ([frame packet exchange](frame-packet-exchange.html);
the audit: [cross-thread engine reads](cross-thread-engine-reads.html)): every render-thread read of engine
memory becomes a read of a COPY the game thread published, handed over through one wait-free
four-slot exchange. Landing 1 builds the exchange, publishes a header-only packet, moves the one
render-thread read whose lifetime no note could establish — the marker text's font — into it as
bytes, and starts the build rule that keeps the rest from growing back. No world pass reads the
packet yet (landing 2: the view and the commands; 3: units; 4a–c: effects, fog, the GL UI's render
half).

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0` | **observer** (`tagpu_detour_observe`, stolen `81 EC 14 02 00 00` — the six bytes `tagpu_menu.c` already observes; chained AFTER it, because the menu's `before` never hijacks the return and ours must). Every action gated on the return address **`0x4969D2`**, the in-play frame callback's call: `before` reserved for the commands (a no-op today), `after` = the publish, post-flip, **only when the cell holds no fresh packet** — the engine draws 330..4900 times a second against ~60 presents, so most draws cost one relaxed load (`skip=`) | game |
| `0x497C70` | **observer** on the loader thread's entry (stolen `55 8B EC 6A FF`, position-independent): logs the thread's id, the load-flag word and, at its return, whether the level's first in-play packet had already been published — the direct measurement of the loader thread and of the ORDER the in-play gate rests on ([engine map](exe-reverse-engineering.html), "The in-play publish point") | loader |
| hook 8, `0x469BD7` (markown's stub) | `tagpu_packet_pub_font_snapshot()`: whenever `[globals+0x204]` or its header signature changed, copy the font's header and its 95 printable glyphs into a game-side buffer, each as a one-glyph font object the blitter accepts; latch `[globals+0x208]`. Every packet carries the buffer (1612 B for the stock in-game font) | game |
| the teardown post hook (`tagpu_reclaim.c`, inside the `0x491B60` wrap) | `tagpu_packet_pub_level_end()`: a header-only packet with `in_game = 0` and the bumped generation, forced past the fresh gate, before the reader is released — without it the renderer would draw the dead level's last packet over the menus | game |
| `0x491B60`, the teardown, **only when `tagpu_reclaim` is not armed** (`tagpu_reclaim.off`, or its own install refused) | **observer** (stolen `A1 E8 1D 51 00`, the five bytes reclaim's wrap takes; hijacked return, so both of the function's exits — the `ret` and the tail-jump to `0x450DD0` — reach `after`): the same level-end packet, published by us. The out-of-game packet must not depend on another module being armed (landing review); with neither provider the publisher stays count-only, because no packet is better than a stale one. The launch line says which: `level-end packet by tagpu_reclaim's teardown post hook` or `by our own observer …`. **[SUPERSEDED 2026-09-12 by landing 3]** this line used to end "under `tagpu_reclaim.off` the level generation stays 0 for the session"; the publisher owns the counter now and advances it here too, which is what makes the template caches drop between levels with reclaim disarmed | game |
| `render_ogl.c`, around the overlay | `tagpu_packet_acquire()` once, before `tagpu_reclaim_pass_begin`, the pointer handed down through `TAGPU_FRAME.packet` / `TAGPU_FXVIEW.packet`; `tagpu_packet_frame_end()` after `pass_end`, unconditionally — the tail check and the heartbeat | render |
| `tagpu_text.c` | `tagpu_text_frame(packet)` copies the font area out of the packet once per font generation; the measure walks the glyph table, the raster hands each one-glyph object to `0x4CCF60` at the x the engine's own string loop would reach. **No `IsBadReadPtr`, no engine pointer on this path any more**; the GL UI's glyph cache (`tagpu_text_glyph`) keeps its probes until 4c | render |

**The primitive, in one paragraph.** Four slots — W (the producer fills it), READ and PREV (the
consumer holds them), and the one in the CELL, fresh or stale. The cell is one aligned dword,
`idx (2 bits) | FRESH`, reserved bits asserted zero. Each side exchanges a slot it holds into the
cell and takes what was there (`__atomic_exchange_n`, ACQ_REL — NOT mingw's `InterlockedExchange`,
whose contract is acquire-only), so the roles stay a permutation without a lock, provided the init
made them one: explicit at DLL attach, `W=0, cell=1 (stale), READ=2, PREV=3`, because zeroed
statics would put both threads on slot 0. `head_seq` stored before the fill, `tail_seq` after;
the consumer latches the head at acquire and compares the tail at frame end. Slots are 8 MB of
address space each, reserved once, committed as the high-water mark rises on the producer's own
slot, never moved, never freed; a fill that does not fit truncates this frame and the next publish
grows first. Neither side ever waits. Every violation is counted, logged rate-limited, never
fatal: thread identity both sides (the render thread's restart across a display-mode change is
recorded, not refused — ownership is by role), the permutation after every exchange, `head ==
tail`, the structural bounds of every offset against the slot's committed size, a canary past the
capacity, one acquire per frame, the tail at frame end, the CRC under `check`.

**The chain rule, enforced.** `tagpu_detour_observe` chains a second observer onto a site by
turning the earlier stub's copy of the stolen bytes into a jump, so the `before`s run in install
order and each sees the engine's own stack — *unless* an earlier observer hijacks the return, in
which case a later one reads that stub's trampoline where the return address should be, and a gate
like ours on `0x4969D2` would silently never match. Since the landing review `tagpu_detour_observe`
refuses to chain onto a stub that has an `after` (returns 0, arms nothing): the hijacker is the
last observer on its site by construction, which is why `tagpu_packet_pub_init` runs after
`tagpu_menu_init` in `dllmain.c`.

**The build rule** (`tools/thread-split-check.sh`, a prerequisite of `ddraw.dll` in
`tagpu/ddraw/Makefile`, so `make -C tagpu/ddraw` and the CI job both fail on an offender; the
upstream `build.cmd`/vcxproj do not run it). A source not on `tagpu/ddraw/thread-split.allow` may
not name an engine virtual address (`0x4xxxxx` or `0x5[0-2]xxxx` — `.text` from `0x401000`,
`.rdata`/`.data` and its bss to the `.tls` at `0x52C000` — suffix-aware, because the `0x00511DE8u`
spelling 19 files use defeats a `\b`), include `inc/tagpu_engine.h`, probe with `IsBad*Ptr`, or
add an offset to `ta`/`main`/`main_p`/`cta`; comments and string literals are stripped first (a
log line naming an address reads nothing; the include is detected before the strip). It scans
`src/*.c`, `src/*.h`, `src/*/*.c` and `inc/*.h` minus the third-party GL and D3D headers. The
list started FULL — **42 files** after the merge with main (its `tagpu_hud.c` joined as a
publisher), each with its class (`publisher`, `session-reader`, `fenced`, `tooling`,
`pure-engine-code`, `engine-map`, `to-convert:N`) and its argument — and only shrinks. Verified:
a planted `*(int*)(*(char**)0x00511DE8u + 0x38A47)` in `tagpu_fps.c` (and again in `tagpu_cfg.c`)
fails `make`; removed, it passes. **What a text rule cannot catch, said plainly:** a main pointer
under a new name plus a macro offset, an address assembled from split macros, an address that
arrives at run time. It is a ratchet against the spellings in use, not a proof; a new spelling is
a review matter.

**Read it in `tagpu.log`.** `packet: ARMED 4 slots x 8 MB reserved, 127 KB committed each …` (the first 64 KB grain plus the
grain the canary's four bytes tip it into) and
`packet: publisher ARMED on DrawGameScreen 0x468CF0 … level-end packet by … loader-thread observer
at 0x497C70=1 …` at launch; then every 300 frames
`packet: pub= skip= overrun= foreign= acq= taken= gap= grow= commitfail= trunc= viol= pviol=
crcbad= nopkt= | pub/s= taken/s= pubus p50= p99= | seq= tick= tps= speed= paused= in_game= gen=
flags= eye= vp= flips= font= fg= trunc= used= | draws= inplay= draws/s= inplay/s= foreign= deep=
fontcopies=<copies>/<refused> levelend=reclaim|own|none`.
`viol`, `pviol`, `crcbad`, `foreign` and `commitfail` must stay 0; `skip` is the fresh gate
working; `overrun`/`gap` are 0 in play and count only under `stress` or across a level end;
`tps` is `GameTime` per wall second and must read 3 × `speed`. A level change logs `packet: level
end -> gen N …`, then the loader thread's entry and exit and `packet: level gen N: first in-play
packet …`, in that order. Levers, read at attach: `tagpu_packet.off` (no slots, no publish, no
acquire; the observer stays in count-only mode so `draws/s` is still reported — **every string
through `tagpu_text_place` draws nothing under it**: the group digits, the `ShowRanges` labels and
the FPS readout, since the render thread has no font), `.check` (CRC-32 per packet),
`.stress` (publish on every draw with a garbage pre-fill, one-page slots that must grow, the
consumer sleeping 0..50 ms per take), `.poison` (the slot handed back is memset), `.show` (a
`PK<seq> T<tick> E<eye>` row under the FPS readout).

**The gates, measured 2026-09-12 on the reference setup, `200v200` at 1920×1080, `--maxfps 0`,
the play defaults:**

- *Protocol.* `check` + `stress` + `poison`: **10 205 taken frames, `viol=0 pviol=0 crcbad=0
  foreign=0`**, 639 181 publishes at 2 906/s (every in-play draw, the overrun path 628 975 times),
  `head_seq` monotone (626 778 sequence numbers skipped and counted as `gap`, never a step back),
  each slot grown once past its one-page start (`grow=4 trunc=1 commitfail=0`); publish p50 12 µs,
  p99 18 µs with the 16 KB garbage pre-fill and the CRC inside the timed region.
- *Cost.* `--maxfps 0`, the camera pinned, one 5-second heartbeat per sample. Armed, sim running: in-play
  `DrawGameScreen` calls/s **795, 826, 795, 847 (mean 816)**; one publish per presented frame
  (`pub/s` = `taken/s` = 307–314, the render thread's own rate uncapped), **publish p50 2 µs,
  p99 2 µs**; the in-play tick rate `tps` **59.79, 60.93, 60.40, 59.01 (mean 60.0)** at the live
  speed 20, i.e. 3 ×. Armed, the ARMOPT menu open (`paused=1`, the tick frozen at 2814 across a
  3-second peek): draws/s **741, 834, 846, 857, 902, 870 (mean 842)**. `tagpu_packet.off`
  (count-only observer, no publish, no acquire), sim running: **795, 770, 845, 783 (mean 798)**;
  menu open: **741, 790, 800 (mean 777)**; the tick frozen at 2815 across the same peek. So the
  armed arm is not slower in either state — it reads 2 % (running) and 8 % (menu) HIGHER — and the
  spread of the 5-second samples (±6 %) is wider than the 3 % band the gate named; the publisher's
  own cost is the measured 2 µs × ~310/s, 0.06 % of the game thread's second. The off arm's tick rate, by two `peek`s 20 s apart: **60.61/s** at the live speed 20 — the same 3 × as the armed arm's 60.0.
- *Levels.* two games in one process (`Tab → EXIT → MAINMENU → CHOICE1`, then `SINGLE → Skirmish →
  Mapping 1 → Start`). The log, in order: `reclaim: level teardown (gen 0 -> 1)`, `reclaim:
  teardown post: freed 279 block(s)`, **`packet: level end -> gen 1: in_game=0 published; 86137
  in-play draw(s) this level; load flags 0x0000`**, `native: level 0 -> 1`, the render thread
  restarted and the GL context changed, **`packet: loader thread 592 entered 0x497C70 (entry #2;
  load flags 0x0001; level gen 1)`**, **`… leaving 0x497C70 (load flags 0x0003 …; first in-play
  packet of this level not yet)`**, **`packet: level gen 1: first in-play packet at draw #86138
  (tick 0, load flags now 0x0003 …)`**, the context changed again. `viol=0` across the boundary;
  the first heartbeat of gen 1 carries `gen=1`. (The second game began PAUSED — `tick=0
  paused=1` — because the first was left in the ARMOPT menu; the engine's, not ours.)
- *The rule.* The planted offender above.
- *Text.* `one-unit` at 1920×1080, the commander selected, put in group 1 (the digit) and `+showranges`
  typed (nine def-range and three weapon circles, each labelled — `build distance`, `mincloak`,
  `weapon1 range`, `weapon3 range`, `sight` …), SHIFT held for the order block, the pointer parked,
  two `glshot`s 4 s apart per DLL. Noise floor (same DLL, 4 s apart): **0 differing pixels** outside
  the top-left readout corner, both DLLs. Previous DLL (`ddraw.dll` built from `1cb60b8`) against
  this one: **0 differing pixels** outside that corner — the digit's own 130×120 rect included —
  and the corner differs only by the readout's numbers and the new `PK` row. So the glyphs
  rasterised from the copy are the glyphs the engine's font pointer gave, pixel for pixel.
- *The two engine facts.* The loader thread's id is now measured at its entry (756 against the
  game thread 736 on the first run), and its exit is logged before the level's first in-play
  packet. Bits 0 and 1 of `main+0x38D75`: no instruction clears them; the packet's per-frame copy
  reads `0x0003` at the first in-play draw and `0x0000` a few seconds later, so a bulk store
  zeroes the word after the load, and the level-cycle run shows the word at **`0x0000` at the level end and `0x0001` at the next
  loader's entry** (bit 0 freshly set by the game thread, bit 1 clear), so a second game in one
  process starts from zero and the loader's bit 1 is a fresh fact each time — the ordering argument
  holds for every level, not only the first.

**Not closed here, by design.** No world pass reads the packet: the eye is still read from engine
memory by every pass and written by the zoom from the render thread (landing 2, with the
commands — the two must land together or zoom-to-cursor wobbles). The unit array, the fog grid,
the effects arrays and the GL UI's render half still read engine memory on the render thread and
say so on the allow-list (`to-convert:N`). The GL UI's string op still hands the render thread a
font pointer (4c). The `tick_start` stamp and `prev` are carried but unused until the lerp
rekeys onto them (3) — and until then the give-back is the plain one, so `prev` MAY carry the same
tick as the packet returned; the tick-aware give-back the plan's §5 describes lands with the lerp
that needs it. Under `tagpu_reclaim.off` the level generation never moves (0 for the session), so a
cache keyed on it would not drop between levels; the level-end packet still arrives (our own
observer), and nothing keys on the generation yet. *[The first sentence is landing 1's state;
§2.17 is what landing 2 closed, and landing 3's review closed the generation: it is the
publisher's own counter now and moves with or without reclaim.]*

### 2.17 The frame packet exchange, landing 2 — the view from the header, the commands the other way (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_zoom.c`, `tagpu_vpwide.c`, `tagpu_input.c`, `tagpu_pal.c`) — 2026-09-12

The plan's row 2 ([frame packet exchange](frame-packet-exchange.html) §11): the header becomes
the source of the one-eye record every pass reads, and every render-thread store into engine
memory becomes a **command** the game thread applies. After this landing no file on the render
thread writes a byte of engine memory, and the eye, the viewport, the palette and the gamma
reach it only through the packet.

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0`, the observer's **`before`** (the in-play gate `0x4969D2`) | **the command apply**: take the latest record the render thread posted (a second instance of the packet's mailbox, latest wins, no waiting either side) and write every engine word it names, on the thread that owns them — after whichever of the frame callback's own camera writers ran this frame (the stepper and the scroll poll both precede the draw call, and both can be skipped: the stepper when paused, both under an in-game GUI screen) and before the draw's first read of the eye, which nothing inside `DrawGameScreen` stores ([engine map](exe-reverse-engineering.html), "The commands, applied in `before`"). `tagpu_zoom_apply`: the level → the clamp's flag, the minimap rect's scale, `ScrollSpeed` at base/z; a NEW record's anchor delta (`cum − applied`, consumed exactly once) → the follow released first when the gesture asked (`0x41C390`'s three stores), then eye and scroll target stepped together; the `tagpu_eye.txt` hold clamped into the camera range and written when it differs; the range re-applied while a zoomed world is live (the walk home after a zoom-out); any eye moved → the minimap box through `0x466B70` and **bit 3 of `main+0x14281` cleared**, as `0x41CB6B` does. `tagpu_vpwide_apply`: the rect widened to the transform's range at the commanded level, or restored; W/H never written, a disagreement counted. Under `tagpu_packet.off` nothing is applied: the engine keeps its own range, rect and rate ([engine map](exe-reverse-engineering.html), "The commands, applied in `before`") | game |
| the observer's **`after`** | the packet grew: `vp_addr` (the rect the engine can name, `+0x37E27..`), `pal` (1 KB, `main+0x143A7`), `gamma` (`[0x51FBD0]+0x614`, bounded), `cmd_ack_seq`/`cmd_ack_dx`/`cmd_ack_dy` (what the apply had done by this draw), `zoom_applied` | game |
| the level teardown (the packet's level-end provider) | `tagpu_zoom_level_end` + `tagpu_vpwide_level_end` before the out-of-game packet: the range flag cleared, `ScrollSpeed` restored to the base, the true rect restored — the shell inherits nothing | game |
| `tagpu_overlay.c`, once per frame before any pass | `tagpu_zoom_read_lever(packet)`: the levers and the wheel's ease as before; the cursor anchor's step is pre-clamped against the same range computed from the packet's copy of the map size and the true viewport and added to a cumulative sum; the **predicted eye** = the packet's eye + the sum the packet has not acknowledged, clamped — what the native pass, the scaffold and every gather draw from (`tagpu_zoom_predicted_eye`). No packet, or an out-of-game one, is no world drawn | render |
| `tagpu_zoom_frame_end`, every exit path of the overlay frame | posts this frame's record: the level and whether a zoomed world is live (the game thread derives the range, the rect and the scroll rate from the pair), `eyeoff`, the cumulative delta, `drop_follow` (a delta is still unacknowledged), the hold from `tagpu_input_cmd` | render |
| `tagpu_native.c`, `tagpu_scaffold.c`, `tagpu_gui_surf.c` (the layer draw), `tagpu_mark.c` (the build-cursor gate), `tagpu_pal.c` | read `vp`, the predicted eye, `vp_addr`, `pal`/`gamma` from the packet; `tagpu_vpwide_true_rect()` is game-thread only now; `TAGPU_FXVIEW` lost its `ta` field and the passes that still walk engine memory (units, features, effects — landings 3, 4a) read the main pointer in their own file, where the rule sees it | render |

**Prediction, and why there is no wobble.** A wheel notch is drawn on the frame it happens: the
render thread adds the step to its cumulative sum and draws from `eye + (cum − ack)`. The game
thread applies the same difference before its next draw and the packet that follows carries the
new eye with `cmd_ack_dx/dy = cum`, so the unacknowledged part goes to zero the moment the truth
arrives — prediction replaced, never added twice. The heartbeat's `unacked=(dx,dy)` reads (0,0)
at every steady state. The step is pre-clamped on the render side against the same range
(`range_pk`, the packet's `vp` and `map_pxw/h`, the same fields `zoom_eye_range` reads), so the
game thread's clamp fires only when the engine itself scrolled the eye to the edge in between,
and then the packet reconciles it like any other engine camera move. A frame whose predicted eye
is ahead of the packet's takes the WIDE fog grid (`tagpu_zoom_unacked`), for the reason the old
handshake did: the engine's grid spans the packet's eye and its slack is 32 px at 1×.

**What went.** The render thread's stores of the eye, the target, the minimap rect, the viewport
rect and `ScrollSpeed`; the fog request/ack pair (`tagpu_zoom_fog_pending/seq/ack`) and
`terr_fogtick`'s OR of it; the follow-release request (`s_dropFollow`, `tagpu_zoom_follow_tick`);
the "residual kept whole while it waits" for the follow; the anchoring gate on terrown owning
the fog draw (the invalidation is the engine's own mechanism now, on the owning thread);
vpwide's W/H repair; `tagpu_zoom_eye_range`/`tagpu_zoom_eye_moved`; `tagpu_pal.c`'s two engine
reads; `tagpu_input.c`'s engine reads and writes; `TAGPU_FXVIEW.ta`. The allow-list lost
`tagpu_input.c` and `tagpu_pal.c` (40 files listed) and re-classes `tagpu_zoom.c` and
`tagpu_vpwide.c` as publishers.

**Read it in `tagpu.log`.** The `packet:` heartbeat gained `addr=(L,T,R,B) z=<applied> pal=1
gamma=` in the packet segment and a `cmd:` segment: `cmd: post= take= new= overrun= viol=
nocmd= seq= ack= unacked=(dx,dy) cum=(x,y) epoch=<seen>/<packet's> z= live= hold=` — `post` per
render frame, `take` per in-play draw, `new` the records actually new, `overrun` the posts nobody
took (all of the shell's, a handful in play), `viol` must stay 0, `unacked` (0,0) whenever no
gesture is in flight, `cum` the sum posted since the epoch began (it reads (0,0) again after a level
end) — and `vpapply= vpwh= applyus p50= p99=` at the end (`vpwh` must stay 0; `applyus` is the
whole apply, both modules, timed with the performance counter like the publish). The `.show` row reads `PK<seq> T<tick>
E<x>,<y> A<ack> D<dx>,<dy>`.

**The gates, measured 2026-09-12 on the reference setup, 1920×1080, `--maxfps 0`, the play
defaults, this DLL against the one built from landing 1's tip (`72772cf`):**

- *The zoom oracles* (`crowd-static` on Two Continents, viewport 128,32 1792×1016, `c =
  (1024,540)`, eye reset to (1728,702) between cases): control +6 at the centre **(0,0)**;
  upper-left (400,300) +6 **(−272,−105)**; lower-right (1700,900) +6 **(294,157)**; zoom out
  (400,300) −6 **(481,185)** — each exactly the predicted `(a−c)(1/z₀−1/z₁)`, each round trip
  back to **(1728,702)** with the level at exactly 1.000. `ScrollSpeed` 32 → 18 at 1.772, 57 at
  0.564, 32 back; the viewport rect (128,32,1919,1047) → (−769,−477,2815,1555) at 0.564 and back.
- *The hover oracle*: the pointer parked on ARMPW unit 1 (screen 600,188), `main+0x2CBA` reads
  `0xFFFF0001` at 1×, after +6 (1.772), after +9 (2.358) and after −9 back; the centre-anchored
  control puts unit 53 there.
- *The follow release*, measured twice. Paused (second level, the commander followed with Ctrl+C,
  tick 0): −4 notches at (900,600) moved the eye by exactly **(58,−28)** and zeroed
  `main+0x142F3`, with the release logged once; +4 at the centre moved nothing and left it set.
  And **with the sim running** (`one-unit`, tick advancing 60/s, after the review pointed out
  that a paused game never runs the stepper the claim is about): Ctrl+C eased the camera onto
  the commander at (705,1047); −4 at (900,600) moved it by exactly **(58,−28)** to (763,1019),
  zeroed the slot, and **three seconds and 750 ticks later the eye was still (763,1019)** —
  the stepper found no follow to ease it back to.
- *The apply's cost*: `applyus p50=2 p99=2` on every heartbeat of every run above — the whole
  apply, both modules, per in-play draw.
- *The epoch*: across the level cycle the render thread's sum read `cum=(58,−28)` at the end of
  the first level and `cum=(0,0) epoch=1/1` in the second, `unacked=(0,0)`, no camera jump at
  the new level's first draw.
- *Minimap rect A/B at zoom*: `main+0x142CB` at the same pinned eye, by the file lever — **1.0
  (27,6,43,15), 0.5 (19,2,51,20), 2.0 (31,8,39,13), back (27,6,43,15) on both DLLs**, the
  viewport rect and `ScrollSpeed` identical at each level too.
- *The repair counter*: `vpwh=0` over 305 779 in-play draws (two levels) and 1 571 648 under the
  stress lever.
- *Protocol*, re-run because the primitive changed: `check`+`stress`+`poison` on `200v200`,
  **21 427 taken frames, `viol=0 pviol=0 crcbad=0` on both exchanges**, `grow=4 trunc=1
  commitfail=0`, head_seq monotone, `vpwh=0` over its 1 571 648 in-play draws; publish p50 18 µs / p99 24 µs under stress, **2 µs / 2 µs**
  in play with the 1.6 KB header (1 KB of it the palette).
- *Levels*: two games in one process — `packet: level end -> gen 1`, the loader entering with
  the flag word at `0x0001` and leaving at `0x0003` before the level's first in-play packet,
  `viol=0` across, the second game's heartbeat at `gen=1 live=1`.
- *The camera hold*: `tacli eye` at (900,400) → eye and target (900,400); at (20000,20000) →
  clamped to **(8928,11656)**, the map minus the view; released, the eye stays.
- *Pictures*: `glshot` at 1× and at 0.564 anchored at (400,300) — the whole island at the
  lower level with the units held under the pointer, nothing torn.

**What the landing review changed (two Opus reviewers, `high`, 2026-09-12; both could construct no
interleaving that breaks the mailbox or loses a delta).** Five real defects, all fixed on the branch
before landing: the camera hold outlived its file by up to 15 render frames (the hold is a level
the game thread re-applies every draw, and `s_holdValid` was cleared only by the 15-frame poll —
now every early exit of the file read drops it); one absurd number in `tagpu_eye.txt` made the
record's validator refuse the WHOLE record and with it every other command (the hold is clamped
at its source now); a notch in a level's last frames could be applied to the next level's camera
(the command apply's **epoch**: bumped at the level end with the applied sum reset, carried in
the packet, echoed in the record — an older epoch's record carries no delta, and the render
thread resets its sum when it sees the new one); the wide-fog gate missed the case where the
range walks the eye home ahead of the packet (`s_unacked` is now "the drawn eye differs from the
packet's", whatever moved it); and the ordering claim was over-stated (above). Also from the
review: the count-only log lines said only that text goes blank; the UI layer's viewport rect
keeps the last in-game one on a frame with no in-game packet (an empty rect would have shown the
key fill raw for one frame at a level's tail); two stale comments in `tagpu_fogwide.c` and
`tagpu_mark.c`; the roadmap's row contradicted itself; two rows of the engine map had the
save-site strings swapped and the third copy site one instruction late. One residual named and
accepted: the message thread's ring test (`to_engine`) mixes the render thread's current level
with the addressable rect the game thread derived from a record up to one render frame old, so a
click exactly at the ring's edge during a wheel ease can be judged against a rect one frame
stale — before the landing both came from the same frame; the true-viewport gate (world or UI)
is unaffected.

**Not closed here.** The unit array, the fog grid, the effects arrays and the GL UI's render half
(the minimap surfaces, the cursor sprite, the string op's font pointer) still read engine memory
on the render thread — landings 3, 4a, 4b, 4c. A render thread that stops posting leaves the game
thread re-applying its last record's levels (the zoom, the hold), the same standing as the render
thread ceasing to write those words itself before this landing; there is no staleness rule, by
design (a count-based expiry is timing). `tagpu_packet.off` now means no world pass at all —
every pass reads the packet's view — and no commands applied: the engine's own camera range,
rect and scroll rate, and after ~90 frames its own terrain again once terrown's skip expires.

### 2.18 The frame packet exchange, landing 3 — the world in the packet (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_native.c`, `tagpu_scaffold.c`, `tagpu_mark.c`, `tagpu_overlay.c`, `tagpu_lerp.c`, `tagpu_order.c`, `tagpu_feat.c`, `tagpu_render3do.c`, `tagpu_posebake.c`) — 2026-09-12

The plan's row 3 ([frame packet exchange](frame-packet-exchange.html) §11): the units, their
pieces, the wrecks and the feature anchors cross in the packet, and **the audit's one open hazard
closes with them** — no file on the render thread walks the unit array, and none dereferences an
Object3do ([cross-thread engine reads](cross-thread-engine-reads.html) §5 row 2).

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0`, the observer's **`after`** | the packet grew four tables. `PK_UNIT` (100 B) one per LIVE unit, the walk bounded by the engine's own SLOT COUNT (`u16 main+0x14351`) rather than by the `begin`/`end` pair, and the table sized to that count so no player is ever cut; `PK_PIECE` (24 B) the COB move and turn triples and the flags, plus the TYPE's template node; `PK_WRECK` (44 B) the husks the anchors name, each carrying the ANCHOR TILE it was found on rather than its own position, because that is what the grid walk it replaced filtered on; `PK_ANCHOR` (16 B) the feature cells of the widest zoom rect with the six height bytes the engine's 2×2 projection average and the Classic++ ground gradient need. The header grew with them: the GUI colour table, the dispatched mouse point, the build cursor's corners, the two mode bytes, the option byte, the feature sweep, the two array COUNTS the fenced passes bound by, and the engine's 32×256 shade table — the per-map BASES are read live instead, because the teardown nulls them and a held copy would not know. Every offset and every bound: [engine map](exe-reverse-engineering.html), "What the frame packet's publisher copies" | game |
| the same `after` | `tagpu_native_owns_unit()` is asked here, once per unit, with the def in hand — the answer crosses as one flag bit, so the unit pass, the marker pass and the composite wipe act on ONE answer instead of three threads' reads of the same bytes | game |
| the order-marker snapshot `0x469BFC` | every unit pointer a record carried becomes an ARRAY SLOT, and every def and weapon field the drawing needs becomes a number in the record: the nine labelled ranges, the three live weapon ranges and their AoE and attack runs, the build def's five footprint extents, the kamikaze set, the cursor sprite's GAF sequence | game |
| `tagpu_packet.c`'s acquire | **the rotation**: the frame instance holds THREE slots (READ, PREV and a SPARE) rather than two, so a packet of the same tick replaces READ and keeps PREV, and the pair the pose blend runs over always spans two distinct ticks. The choice is made AFTER the exchange out of records this thread owns — the two-slot alternative would have to peek at a slot the producer may be refilling | render |
| `tagpu_native.c` | the unit and wreck gathers, the whole pose path (`pose_accum_body`, `posed_pose`, `hires_pose`, `pose_dump`, the pose CRC), the ownership test, the ground height, the option bits, the sub-pixel table's key: all from the packet. What it still reads is per-LEVEL assets under `tagpu_reclaim`'s teardown fence — the model templates behind `MODEL_PTRS`, whose base is read LIVE every call (the teardown nulls it at `0x42DCD8`, and that null is the refusal) while the bound is the packet's `udef_count`, and the screen fog descriptor, which is landing 4b's | render |
| `tagpu_scaffold.c`, `tagpu_feat.c` | the anchors, the map and sweep dimensions, the units they stamp for; the FeatureDef and wreck RECORDS stay, under the fence. Their bases are read LIVE at every use (the teardown nulls both, and those nulls are the refusal) and their indices are bounded without a probe: a def row by `min(live NumFeatureDefs, the packet's)`, a wreck record by the engine's fixed 2048-record pool | render |
| `tagpu_mark.c`, `tagpu_overlay.c`, `tagpu_lerp.c`, `tagpu_hires.c` | off the allow-list entirely: they name no engine memory at all | render |
| `tagpu_render3do.c` | the shade table and the build-state formulas take the packet's copies; the **write-back is deleted** | render |

**The lerp is rekeyed, and its arena is gone.** `tagpu_lerp.c` kept 3.4 MB of pose banks keyed on
`(Object3do, nparts, level generation)` and sampled `P_POS`/`P_TURN` on the render thread. The two
held packets are that history now: `prev` is an earlier tick and `read` a later one by
construction, units are matched by the STABLE ID (`unit+0xA8`) and verified on the model identity,
and the blend's clock is the two packets' own `tick_start` stamps — taken by the game thread the
instant it first saw each tick, within one engine draw of the boundary — instead of the learned,
smoothed period this module used to keep. The arena, the pointer hash, the free list, the ageing
sweep and the `LERP_BLOCK` 48-piece ceiling all go; a 256-piece model is now interpolable.

**What was deleted rather than converted.** The opt-in write-back (`tagpu_render3do()`,
`tagpu_writeback.on`, `writeback_paint`) — it walked an Object3do on the render thread and then
STORED into engine memory from there, which is the one shape the exchange exists to remove;
landing 2's "no render-thread store into engine memory remains" was true only because it was off
by default. With it went its FBO, its program and the four readback planes. Also gone: the model
probe in `tagpu_overlay.c`, and `tagpu_scaffold.c`'s whole-map feature census (`tagpu_features.trigger`
has answered that question properly since).

**Read it in `tagpu.log`.** The `packet:` heartbeat's packet segment gained `units= pieces=
wrecks= anchors=<n>(<cols>x<rows>) dup= pair= same=` — `dup` is the stable-id collision oracle over
one packet and **must read 0**, `pair` the frames that had a usable two-tick pair, `same` the
rotations that displaced READ because the tick had not moved. A `world:` segment at the very end
carries the publisher's own: `u= p= w= a=<anchors>/<cells scanned> scan=<scans>/<reuses> dup=
trunc=<units>/<pieces>/<wrecks>/<anchors> relbad= woob= shd=`. **`relbad` must stay 0** — it counts
draws on which `end != begin + (count−1)·0x118`, the relation the note records — and so must every
`trunc` past the first fill of each slot. **`woob` is the wreck-record index the bound refused**,
added by the landing review below; it is a monitor and not the safety argument, which is the
engine's own 2048-record pool.

**What the landing review changed.** Two Opus reviewers at `high` over the branch diff, one
working down the brief's risk list and one told to range freely. **Eight defects**, every one
verified against the pristine binary before anything moved — seven the reviewers' and one found
while verifying their first. All eight are fixed on the branch:

| # | what it was | why it mattered |
|---|---|---|
| 1 | the packet CACHED the three per-map array bases `main+0x1426F`, `+0x1420B` and `+0x14377` | **the worst of the eight.** The teardown frees each and then NULLS it (`0x4221F8`→`0x422214`, `0x42227D`→`0x42228B`, `0x42DCCB`→`0x42DCD8`), and that null is what refuses the walk on the next frame — the code this replaced read live and bailed on `ptr_ok(NULL)`. A copy taken at publish time is a pointer to freed memory that still tests non-NULL, so the fenced passes read straight through it for the length of a teardown, and with `tagpu_reclaim` unarmed for the whole cascade. The bases are read live at every use again; only the BOUNDS stay in the packet |
| 2 | the wreck-record index reached memory **with no bound**: the feature cell's raw `u16` times `0x30` off `main+0x1420B` | up to ~3 MB past the pool. A garbage "Object3do" that survives `ptr_ok` puts up to 256 rows of nonsense `node` pointers into the packet, **dereferenced afterwards on the render thread**. The engine's own read at `0x46A6C4` is unbounded too, but it only forms the address for a cell it is DRAWING, where this walk covers the zoom-floor rect plus a 32-cell margin. The bound is the engine's: `0x421F29` allocates `0x18000` bytes at stride `0x30`, so **2048 records** |
| 3 | `level_gen` was `tagpu_reclaim_level_gen()`, bumped only in reclaim's teardown post hook | with reclaim unarmed it never moved, so the PREV withheld across a level, the pose blend's refusal and **all four model-template caches** silently stopped invalidating. The publisher owns the counter now |
| 4 | an **in-range** permutation break was counted and carried on from | the cell held a slot the consumer already holds, so the producer wrote a slot it did not own — the partition the whole exchange rests on is gone, and continuing leaves a duplicate in the held set and one slot owned by nobody. It fail-stops now, like the out-of-range case |
| 5 | `tagpu_order.c` returned early on the explode RADIUS where the engine returns on the ExplodeAs weapon POINTER (`0x439125`) | dropped the second circle (`0x439196`) for a weapon whose `AoE>>1` is 0. Inert on stock content |
| 6 | a truncated anchor scan was cached as a complete one | every reuse publish of that tick under-reported `trunc` — 6 publishes in 7 |
| 7 | the wreck gather left the nanoframe group of the static `units[]` unwritten | a wreck on an index that held a unit under construction was staged and wire-drawn as one. **Present on main since G16**, not introduced here |
| 8 | *(found while verifying 1)* the FeatureDef index was bounded by the PACKET's `NumFeatureDefs` against a base read live, and two `IsBadReadPtr` probes stood in for the bound — one on the wreck record in `tagpu_feat.c`, whose index was as unbounded as the publisher's, and one on the FeatureDef record | across a level boundary those are two different maps. The engine grows that array one record at a time and writes the count **last** (`0x422543` reallocs, `0x422558` stores the base, `0x422DAC` increments the count), so the live count is always a conservative bound on the live base — the two passes take `min(live, packet)` now. `MODEL_PTRS` is the opposite order (`0x42D542` counts, `0x42D693` allocates, `0x42D6AA` stores), so a non-NULL base there already implies its count, and the packet's `udef_count` stands |

One more was a DOCUMENTATION finding and is acted on rather than fixed: every sentence in these
notes that called a per-level read safe "under `tagpu_reclaim`'s teardown fence" was true only
while reclaim is ARMED, and reclaim has five ways not to be. [Thread-safe
destruction](thread-safe-destruction.html) §10b now carries the residual in full — what the live
bases and the new bounds do cover (a pass that starts after the teardown's null-out, and any index
straying outside the block), what they do not (a pass already inside the walk when the cascade
frees under it), and why there is no by-design fix in this landing's shape. It is an **open**
residual whose retirement is the plan's row 5.

Rejected: one PLAUSIBLE finding that the anchor rect's left margin is thin at the zoom floor. The
detector already exists — `outside=` on the `feat:` line, a flag set when the frame's rect asks for
cells outside the ones the packet carried — and it reads **0 on all 116 frames** of two runs at
`z=0.25`, the floor, with the eye thrown to nine map corners and back four times each, on a
forest fixture (`anchors=541` of 98 736 cells) and a wreck one (`anchors=730` of 142 136). Not
asserted this time: measured.

**The gates, measured 2026-09-12 on the reference setup, 1920×1080, `--maxfps 0`, the play
defaults, this DLL against the one built from landing 2's tip (`4b84098`):**

- *The protocol*, `200v200` under `check`+`stress`+`poison`, three runs of 300 s: **1 271 008
  publishes and 19 481 taken frames** (436 191/6 658, 376 318/6 177, 458 499/6 646),
  `viol=0 pviol=0 crcbad=0 foreign=0 commitfail=0` on both exchanges in all three,
  **`dup=0`** (the stable-id collision oracle) and **`relbad=0`** (the `end` pointer agreed with
  `begin + (count−1)·0x118` on every published draw), 0 `VIOLATION` lines in the log. **Re-run once
  on the binary that ships** after the review's fixes — which changed the exchange itself, adding a
  second fail-stop — for another **450 345 publishes and 7 377 taken frames** with the same five
  counters at 0, plus the new `woob=0`, `raced=0`, and `pubus p50 148 / p99 290 µs` under stress.
  `trunc=0/1/0/0` in every run is the growth path doing its job: the pieces slot truncates while it
  is still growing (`grow=22`) and never afterwards.
- *The pose CRC join* (`tagpu_posecrc.on`): **`raced=0`**. That is the gate this landing is
  measured by and its meaning changed with it — the race it counts is between the COB scripts on
  the game thread and the pose loop on the render thread, and the pose now comes out of a packet
  the game thread finished writing before it handed the slot over, so a non-zero count would mean
  the exchange itself is broken rather than a rate to accept.
- *The pose, held to the previous DLL directly*: on `selbox-slope` (three ARMSTUMPs on a hillside,
  real bank and pitch in the angle triple) the three units' `in`/`out` CRC pairs are
  **identical in both builds** — `42db193f`/`76a8ee95`, `65210a9c`/`22fc162a`,
  `40efe2fd`/`95f837bd` — and so is the whole `native:` counter line. The matrices, the shade
  rows and the visibility words the pass hands the GPU are byte-for-byte what they were.
- *Two level cycles in one process, three levels in all, run once with `tagpu_reclaim` ARMED and
  once with it OFF* — because the review's third finding was that the level generation only moved
  in the armed case, and the two providers of the level-end packet are the two halves of this gate.

  | | reclaim ARMED | `tagpu_reclaim.off` |
  |---|---|---|
  | provider on the heartbeat | `levelend=reclaim` | `levelend=own` |
  | the generations | `gen 1 (reclaim's 1)`, `gen 2 (reclaim's 2)` | `gen 1 (reclaim's 0)` |
  | the template caches | `native: level 0 -> 1` and `1 -> 2`, both dropping | `native: level 0 -> 1`, dropping |
  | `VIOLATION` lines | 0 | 0 |
  | `viol` / `dup` / `relbad` / `woob` / `trunc` | all 0 | all 0 |

  **The publisher's counter is not a divergent second counter**: with reclaim armed it tracks
  reclaim's exactly, and with reclaim absent it is the only one that moves. **The right-hand column's
  template-cache row is the fix.** Before it, with reclaim disarmed, no `native: level` line existed
  at all — a second level reusing a template address served the first level's answer for the life of
  the process.
  The loader thread entered three times and left before each level's first in-play packet, the
  command epoch tracked the generation (`epoch=2/2`), the new level's tables were sized to the new
  map, and the feature pass was unbothered by the new `min(live, packet)` def bound
  (`defs=442 junk=0 outside=0`).

- *Cost*. The publish is **p50 70 µs, p99 322 µs** at 354 units / 5 443 pieces / 35 wrecks /
  760 anchors, against landing 2's 2 µs for a header-only packet; the command apply is unchanged at
  p50/p99 **2 µs**. Two thirds of that first figure was the feature-grid scan (168 µs of 200 before
  it was cached per tick — 465 × 273 = 126 945 cells at the zoom floor), which now runs **735 times
  against 4 567 reuses** over one interval. Under `stress` the publish is p50 136 / p99 324 µs.

  **And it is not slower — measured twice, and the second time on the shipped binary.** On
  `200v200` (≈400 units) with the sim paused and the restore finished, the two DLLs run back to
  back for 60 s each:

  | pair | what | this DLL | `4b84098` |
  |---|---|---|---|
  | first | render frames / s | 207.5 | 178.5 |
  | first | in-play draws / s | 811 | 763 |
  | second, the shipped binary | render frames / s | **134.5** | 128.5 |
  | second, the shipped binary | in-play draws / s | **769** | 684 |
  | both | publish, game thread | p50 112 µs, p99 220 µs | p50 2 µs, p99 2 µs |

  **Read the direction, not the figure.** The method is identical in both pairs; what differs is
  how busy the GPU was, and that alone moves the level by 70 frames a second. So the only claim the
  measurement supports is the one both pairs agree on: the new build is **not slower**, and is
  modestly ahead on both. The publisher's 112 µs is real and is spent on the game thread; what buys
  it back is that the render thread no longer walks the unit array, the feature grid or an
  `Object3do` per unit per frame. This is not a speed landing and the number is not a target — it
  is recorded because a cost this shape could equally have gone the other way, and only a
  measurement tells the two apart.

- *Pixel A/B against landing 2's DLL* (`4b84098`), 1920×1080, `--maxfps 0`, the play defaults, the
  world viewport only, the animating cursor sprite's rect excluded as `uiwalk` excludes it:

  **Two different floors, and a row needs whichever is larger.** Within a launch, consecutive
  frames of a fixture with a burning wreck or an idling unit already differ — that is the
  within-launch floor. Across two launches of the SAME build they differ again and for a second
  reason: Classic++'s unit-atlas restore is lazy and time-sliced against a GPU budget, so the
  atlas it has finished is not the same one twice. On `selbox-slope` the within-launch floor is 0
  and the cross-launch floor is **26 pixels** — three clusters of single-channel ±1 on the three
  tanks, maximum channel delta 3, inside x 702..956, y 456..576. Measured twice, on two different
  builds, it comes back as the same 26 pixels in the same box. Every row below therefore carries
  both floors and the A/B, and the column that decides anything is the last one:

  | fixture | zoom | what it exercises | this DLL's floor | `4b84098`'s floor | A/B | **outside both floors** |
  |---|---|---|---|---|---|---|
  | `selbox-facings` | 1 | three tanks on the flat, the posed unit path | 0 | 77 | 0 | **0** |
  | `one-wreck` | 1 | the wreck table's own path | 0 | 0 | 0 | **0** |
  | `hires-vehicle-slope` | 1 | the glTF replacement pass | 0 | 0 | 0 | **0** |
  | `hires-wreck` | 1 | the replacement pass over a husk | 495 | 833 | 500 | **0** |
  | `selbox-facings` | 0.5 | the same three tanks, wide | 183 | 232 | 189 | **0** |
  | `feat-forest` | 0.5 | the anchor table over a forest | 178 | 165 | 206 | **0** |
  | `selbox-slope` | 1 | three tanks on a hillside, real bank and pitch in the angle triple | 0 | 0 | 26 | 26 — **under its own 191-pixel cross-launch floor** |
  | `cob-kbot` | 1 | a live COB script over a burning wreck | 780 | 414 | 738 | 18 |
  | `pose-inventory` | 1 | 69 units, all eight pose classes, every one idling | 7 140 | 9 194 | 10 126 | 731 |

  **Six of the nine are exactly 0**: every pixel that differs between the two builds is a pixel
  that differs between two frames of the SAME build. For the three that are not, the decisive
  control is this build launched TWICE and held to itself, which is the comparison the A/B actually
  is — and on all three **the build differs from itself by more than it differs from `4b84098`:**

  | fixture | this build vs ITSELF, two launches | this build vs `4b84098` |
  |---|---|---|
  | `selbox-slope` | **191** | 26 |
  | `cob-kbot` | **847** | 738 |
  | `pose-inventory` | **10 457** | 10 126 |

  There is no residue to explain after that, but each has a named cause anyway:

  - **`selbox-slope`.** Its 26 pixels sit in one box (x 702..956, y 456..576, maximum channel delta
    3) that reproduces on every measurement, on two different builds: three clusters of
    single-channel ±1 on three tanks of the same type, i.e. the same atlas texels three times.
    It is Classic++'s lazily restored unit atlas, whose restore is time-sliced against a GPU budget
    and does not land identically twice. With `classicpp` off the two builds agree to **1 pixel**.
    That atlas is also why this fixture's own within-launch floor is not a constant — 0 when the
    GPU was busy and the restore had stalled between two shots 3 s apart, 265 and 288 when it was
    free and the restore was still running.
  - **`cob-kbot`.** Its 18 pixels outside both within-launch floors lie at x 1509..1525, y 318..362,
    wholly inside the 45×65 box that holds the burning wreck's smoke plume — drawn by the particle
    pass, which is landing 4a's and untouched here.
  - **`pose-inventory`.** 69 units all running idle scripts. Its floor has been measured at 7 140,
    8 556, 8 957 and 9 056 within a single launch of one build. It is the pose COVERAGE fixture,
    not a parity one, and what actually holds this landing to the previous DLL there is the pose
    CRC join above: `raced=0`, with identical `in`/`out` pairs.

  **One fixture was DROPPED from the wide-zoom set, and the rosters are why.** An earlier sweep
  ran `one-wreck` at zoom 0.5 and it read 217 pixels outside both floors, 196 of them in the
  rightmost 32-pixel column of the viewport, where the picture is a unit clipped by the edge in one
  build and absent in the other. Reading `tacli roster --json` from both launches settles it:
  **they are not the same game.** One had 8 units and the other 9, the player's own commander
  started at world (3552, 1008) in one and (9283, 5088) in the other, and every AI's commander and
  mex was somewhere else again — **the skirmish assigns start positions per launch.** At zoom 1
  that camera sees no unit at all, which is why the same fixture is 0 there. A wide-zoom fixture is
  comparable only while no unit enters the widened view; `feat-forest` and `selbox-facings` are,
  and they are the wide-zoom evidence here.

  **Two harness traps cost a round of wrong numbers each, and both are worth writing down.**

  *A parity capture must wait for `restoreglsl: terr: done`, not for a fixed settle.* With several
  instances sharing the GPU the Classic++ restore does not finish in 30 s, and two mixed frames —
  part indexed art, part restored — differ by tens of thousands of pixels for a reason that has
  nothing to do with the build. One such pair read **71 558**. The ta-drive skill has said this
  since G14e; the first cut of this sweep did not do it.

  *And the cursor sprite must be masked from where the ENGINE has it, not from where you parked
  it.* `scenario load`'s `center_on` leaves the pointer on the anchor, and an injected `mouse:`
  does not always survive the engine's own polls: in one pair the new build's cursor sat at
  (60,1000) where it was put and the reference's at (950,529), the screen centre. That is a 21×23
  animated sprite in two different places — **195 pixels**, all of it the cursor, and 0 once both
  rects are masked. Read `[0x51FBD0]+0x1B6`/`+0x1BA` per instance rather than assuming.

  What is left after both is the fixture's own floor: **26 pixels** across two launches of this
  DLL, three clusters of single-channel ±1 on the three tanks — the same unit type, so the same
  atlas texels three times over. It is the Classic++ restored unit-atlas twin, whose lazy restore
  is time-sliced against a GPU budget: with `classicpp` off the two builds agree to **1 pixel**,
  the pose CRC join says the matrices, shade rows and visibility words are byte-identical, and the
  `native:` counter line is identical.

- *The rule*: the allow-list is **36 files**, down from 40, with `tagpu_mark.c`, `tagpu_overlay.c`,
  `tagpu_lerp.c` and `tagpu_hires.c` off it entirely and four more re-classed from `to-convert:3`
  to `fenced`; a planted `0x00511DE8` in `tagpu_mark.c` fails `make`.

### 2.19 The frame packet exchange, landing 4a — the effects and the particle layers (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_fx.c`, `tagpu_sfx.c`, `tagpu_fxown.c`, `tagpu_gaf.c`) — 2026-09-12

The plan's row 4a ([frame packet exchange](frame-packet-exchange.html) §11): the projectiles, the
explosions, the flying debris and the ten particle layers cross in the packet. **This is the one
client `tagpu_reclaim`'s per-LEVEL fence never covered** — the layer table is per game, but each
layer's `{begin,end}` pair and every object's sub-particle vector are `std::vector`s the game
thread GROWS mid-play, freeing the old array (`0x4732E0`), so the pair could be read skewed and a
consistent pair could name memory just freed ([cross-thread engine reads](cross-thread-engine-reads.html)
§5). The probes both files carried made that rarer and nothing else.

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0`, the observer's **`after`** | four more tables. `PK_PROJ` (56 B) one per live projectile, with the rendertype's own rotation adjustment applied, the weapon's colour NUMBERS already through `main+0xDCB`, the attacker's owner byte, the ground-shadow blob's world y, and the sprite or flare frame already indexed by this tick; `PK_EXPL` (32 B) the debris node, the two anim states' resolved frames and the turn triple; `PK_DEBRIS` (24 B) one per occupied particle slot; `PK_PART` (16 B) one per drawable sub-particle, **in layer order**, with the projection done (`x` = hi(world x), `zp` = hi(y) − hi(alt)/2) and its GAF frame resolved. The header gained the ALP/LHT capability bits, the ground-shadow frame, the 32×256 LHT ramp and the **whole 256-byte** GUI colour LUT | game |
| the same `after` | **the gather is taken once per SIM TICK**, keyed on `(level generation, tick)`. All four arrays are sim state — the tick moves a projectile, advances an explosion's anim frame and runs every particle's update leaf — and the engine's own draw passes (`0x49BE60`, `0x420B00`, `0x471F90`) read them and write nothing, so two publishes of one tick must produce the same table. Measured: **375 scans against 71 668 reuses** on a paused fixture, ~1 in 190. The LEVEL half of the key is what stops a hit across a boundary handing the native pass a model root the teardown has freed | game |
| `tagpu_fx.c` | the three effect tables and the engine's DRAWING RULES over them — which rendertype makes which primitive, where the shadow blob goes, how the lightning bolt jitters. It reads no engine memory at all and is **off the allow-list**; the model roots it carries are per-TYPE templates it never dereferences, passed straight to `emit_fx_model` in the (fenced) native pass | render |
| `tagpu_sfx.c` | the particle table, walked by layer because the layer IS the draw depth (0..6 before the projectiles, 7..9 after the explosions). Also **off the allow-list** | render |
| `tagpu_gaf.c` | gained `tagpu_gaf_frame_geom` / `_subframe`, so a pass that only PLACES a sprite dereferences no engine byte of its own; the publisher calls its resolvers on the game thread | both |
| `tagpu_fxown.c` | the render thread's standing request for the tables (`want`), with the same 90-frame watchdog the two skip bytes stand on: an unarmed pass costs the publisher nothing | both |

**Three bounds became the engine's own, by disassembly** ([engine map](exe-reverse-engineering.html),
"The effects: the four per-frame arrays"). `0x499A30` allocates the projectile array as `0x7D64`
bytes = exactly **300** slots of `0x6B`, and both append sites refuse past 300 (`0x49B6EE`,
`0x49B809`), so the render-thread pass's 8192-slot sanity cap is gone. The explosion add site
refuses past 300 (`0x420A42`). The debris slots are the 100 dwords at `0x511DF0..0x511F80`. **The
particle layer's bound is 401, not 400** — every emitter reads the size and `cmp eax,0x190 / jbe
append`, and past 400 destroys the FRONT object, shifts the vector down and appends anyway, so 401
is the steady state; a walk that stopped at 400 dropped the whole layer every time it filled, which
is what the new `layerbad` counter read **86** times on one `fx-mix` run before this was corrected.

**Two facts the landing established and the engine map now records.** The **"fx" GAF bank is a
SESSION asset**: `0x429870` loads it and resolves every named sequence into `main+0x147AB..`, and
its only caller is `0x49134D` inside `0x491200`, whose only caller is `0x49EA62` in WinMain
(`0x49E830`) — so the effect sequences are loaded once per process, not per level. And
**`main+0xDCB` is a 256-byte LUT**, not 64: `0x4AC7D0` writes exactly `0x100` bytes of it
(`0x4AC7FF..0x4AC88F`). The packet's `gui_col` grew to match, because a weapon's colour NUMBER
indexes that table and nothing bounds it below 256.

**What the gates measured (2026-09-12, 1024×768, `--maxfps 0`, the play defaults).**

- **A cross-build pixel A/B is NOT AVAILABLE on a combat fixture, and that is measured rather
  than assumed.** Two runs of ONE build on `fx-mix`, both paused at a fixed tick, differ by
  **39 478 px**: the sims reach different states (tick 649 against 655, `proj=3 expl=21` against
  `proj=0 expl=2`). Aligning the pause to a fixed number of ticks after the load does not fix it
  (784 against 810, `sub=130` against `sub=95`). What IS deterministic is one launch: the
  **within-launch floor is 0 px** on every state of every fixture measured.
- **So the effects gate is the ENGINE'S OWN DRAW of the same arrays, in the same launch, at the
  same paused tick.** Three frames: ours (`fx.on`+`sfx.on`), the engine's (both `passive` — it
  draws, we gather and count), and neither (both armed with every class muted, so we own the draw
  and emit nothing). `engine != neither` is where the engine put effects and `ours != neither` is
  where we put them, over one frozen world, with Classic++ off so both come off the same
  palette-indexed frames.

  | fixture | build | our px | engine px | both | IoU | ours-only | engine-only |
  |---|---|---|---|---|---|---|---|
  | `fx-mix` | **4a** | 13 436 | 13 280 | 13 207 | **0.978** | 229 | 73 |
  | `fx-mix` | landing 3 | 8 100 | 8 216 | 8 093 | 0.984 | 7 | 123 |
  | `sfx-strait` | **4a** | 777 | 777 | 777 | **1.000** | 0 | 0 |
  | `sfx-strait` | landing 3 | 1 048 | 1 181 | 1 048 | 0.887 | 0 | 133 |

  The two builds' scenes are not the same scene, so the counts are not comparable and the RATIO
  is. `fx-mix`'s 229 ours-only pixels are three clusters, and the largest was looked at: a smoke
  puff both builds draw in the same place, where our GL alpha blend covers a few pixels the
  engine's `AlphaCompsteBuf2OFFScreen` leaves cyan-fringed. That is the pass's own pre-existing
  character and scales with how much smoke is on screen.
- **The heartbeat's new `fx:` segment**: `proj= expl= deb= part=<this packet>/<high water>
  scan=<gathers>/<reuses> trunc= layerbad= subbad= lht= want=`. `trunc`, `layerbad` and `subbad`
  must all read 0. The particle high-water mark on the fixtures measured was **315** against the
  16 384-entry cap.

### 2.20 The frame packet exchange, landing 4b — the fog grids (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_fogwide.c`, `tagpu_native.c`, `tagpu_terrown.c`, `tagpu_fx.c`) — 2026-09-12

The plan's row 4b: both fog lattices cross in the packet, and **`tagpu_fog_at`'s guard loses its
reason to exist**. The engine's own screen grid (`*(main+0x1421F)`) was read on the render thread —
the descriptor AND the buffer behind it — while this fork's terrain owner was calling the engine's
builder over both from the game thread. That is the read a hard fault off a base of −9 came out of
on 2026-09-03. **The root cause is still not found**; what changes is that the class is gone.

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen`'s `after` | the engine's grid, with its own relation checked (`cells == ((cols*rows + 7) & ~7)`, the allocator's round-up at `0x483C84`) and exactly `cols*rows` entries copied; `tagpu_fogwide`'s wide grid the same way; the grey band's 256-byte palette remap (`*(TAProgram+0xCC)`), latched like the shade and lighten tables | game |
| the engine's fog site, inside the draw | `terr_fogtick` latches the eye the engine's builder actually read, the instant it ran, so the grid's world ORIGIN is taken from that eye and not from the packet's | game |
| `tagpu_fogwide.c` | **one buffer, no lock, nothing retired.** The three buffers swapped under a critical section, the retire ring behind `tagpu_reclaim`'s quiescence fence, the wall-clock liveness test, `tagpu_fogwide_dimcap()` and the `ret=` / `held=` / `strand=` / `bare=` counters are all gone with the hand-over they existed for. A grow frees the old block on the spot | game |
| `tagpu_native.c` | both grids out of the packet; which one this frame uses is unchanged and still this thread's decision, because it is the thread that knows what it is about to draw | render |

**The bound is exact now, not generous.** The acquire checks `len == cols * rows * 2` against the
record's own extent, so the largest index a consumer can form is inside the bytes it was handed —
by construction, where before it was a per-dimension cap the two producers had to agree about (and
once did not: a screen between 4057 and 8153 px wide got a grid the producer built and the gate
refused, answering "nothing is hidden here" for the whole screen).

**The origin is right rather than right by coverage.** The render thread derived cell (0,0)'s world
point from its PREDICTED eye — the packet's plus a cursor-anchor step the game thread had not
applied — and covered the disagreement by taking the wide grid whenever anything was
unacknowledged. It still takes the wide grid there; the origin is now the eye the builder read.

**What the gates measured.** A static fixture is the one shape a cross-build fog A/B is available
on, and `selbox-slope` is one: `shootall: false`, no orders, a pinned camera, the sim paused as
soon as it is live, so the LOS state is a pure function of where the scenario put the units.
Loaded `--mapping 0 --los 1` so there IS fog, at four stops — zoom 1.0 and 0.5, at the camera's own
position and at the map's (0,0) corner, zoom driven through `tagpu_zoom.txt` so the camera never
moves between the first two. **Every stop: within-launch floor 0 px, and 0 px between landing 3's
DLL and this one.** `fogwide check: differ=0` on 720 of 720 cells on both builds; `bare=0`.
The heartbeat's new `fog:` segment is `<cols>x<rows> wide=<cols>x<rows>/<publishes> refused=
shade=`.

**And re-measured on the binary that SHIPS**, after the review's fix put a draw stamp in front of
both fog answers — because that fix could have refused the wide grid outright and the only thing
that would have said so is a picture. Same four stops, same 0 px within a launch and **0 px against
landing 3's DLL**, `bare=0`, `fogwide check: differ=0`.

### 2.21 The frame packet exchange, landing 4c — the GL UI's render half (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_gui_surf.c`, `tagpu_gui_hook.c`, `tagpu_gui_leaves.h`, `tagpu_gui_int.h`, `tagpu_text.c`, `tagpu_overlay.c`) — 2026-09-12

The plan's row 4c and its §9. The GL UI layer is unchanged except in where its render half gets
its inputs; the op QUEUE is untouched and stays a queue. Full detail:
[GUI renderer](gui-renderer.html) §23. In one table:

| what it was, on the render thread, every present | what it is |
|---|---|
| the cursor's position and sprite record through `[0x51FBD0]+0x1B6`/`+0x1BA`/`+0x1B2` | header fields; the record is a KEY into the session cursor table and only `tagpu_gaf.c` dereferences it |
| the minimap's box, its view rect and its colour byte | header fields |
| its three 8bpp surfaces, walked row by row to interleave into RGB | one area the PUBLISHER interleaves, gated on the sharp minimap asking for it (at k = 1 it is deliberately the engine's own) |
| the level's picture, from a buffer the LOADER thread filled | the level's FIRST in-play packet, acknowledged by the consumer; **the loader-thread observer is deleted** |
| `PK_STRING.frame`, the engine's FONT OBJECT, read per glyph behind `IsBadReadPtr` | a font ID and each glyph's width and packed ROWS, copied on first sight of a (font, code) pair on the game thread |

**The loader-thread observer went because a fact in this fork's own notes was wrong.** They said
the minimap picture `main+0x1426B` was alive only inside `BuildMinimapSurface 0x466780`. It is
alive for the whole level: LoadMap stores it at `0x483900`, `0x466780` reads it at `0x46684F`
without nulling it, and the only free is `0x483DFE` inside `0x483DD0`, whose only caller is
`0x491BB3` — the teardown cascade. So the publisher decodes it itself on the level's first in-play
draw, and **nothing of ours runs on the loader thread any more**, which is the second of the two
things the row exists to close.

**Not closed, and named rather than found later: on a SHELL frame the cursor is the engine's own
again.** Its position and sprite were read live out of the graphics globals on the render thread —
in play and on the menus alike — and the publisher only publishes from the in-play gate, so a shell
frame holds the out-of-game packet and reports no rect. At k = 1 the same art; at k > 1 the
engine's sprite blown up instead of ours at the device's resolution.
[GUI renderer](gui-renderer.html) §23 *Not closed here* has it.

**`tagpu_gui_surf.c` is off the allow-list and `tagpu_text.c` carries no probe.** The blitter
`0x4CCF60` is still `pure-engine-code` and is now the only thing on that list from either text
path: both hand it a one-glyph font object of ours, so TA's own glyphs are still TA's own blit.
The list is **33 files**, from 36 before landing 4.

### 2.21c Landing 4c's own gates

The static fixture of §2.20 again — `selbox-slope`, `--mapping 0 --los 1`, paused as soon as it is
live — with `gui.on=mmbase` forcing the sharp minimap on at k = 1, which is the only way the k = 1
comparison can be taken at all (§19). **The whole UI frame: 0 px within a launch on both builds,
and 0 px between landing 3's DLL and this one** — the minimap drawn from packet-carried surfaces
against the same minimap drawn from live engine reads, with `fog=13333/13356` of its texels masked
on both, so the fog mask was actually exercised rather than inert.

The counters, both builds: `curs=1,10x20,dev=1` — **our** cursor, at the 10×20 device footprint
that says ours is drawn and the engine's erased; `str=…,miss=0,reseed=0` — **no glyph the cache
refused that the engine would have drawn**, over ~14 700 presented frames of text; `lost=0
strict=0 overflows=0`. The packet's own segment reads `mm=106x126/14726 refused=0 pic=252x252/2`:
the picture went in **two** packets before the consumer's acknowledgement stopped it, which is the
acknowledgement working, and `noeng=1` against the reference's 0 is the single warm-up frame the
standing request costs.

PLACEHOLDER-WALK2

### 2.21b The protocol and the cost, measured across all three rows

**The protocol**, `200v200` at 1920×1080 under `check`+`stress`+`poison`, on landing 4's DLL and on
landing 3's, back to back: **10 487** taken frames against 10 564, and on both exchanges of both
builds `viol=0 pviol=0 crcbad=0 foreign=0 commitfail=0`, with `dup=0 relbad=0 woob=0` in the world
segment and `trunc=0/0/0/0 layerbad=0 subbad=0` in the effects one. `grow` and `trunc` past the
first fill of each slot are the stress lever's growth path doing its job and are 45/19 against
21/11 — landing 4's record is bigger, so it grows more.

**The packet is bigger, and most of it is one table.** `used=` on `200v200` at 1080p went from
**65 KB** to **168 KB**. Turning the two new gathers off in the same launch says where it goes: the
three effect tables and the particle one are **8 KB** and the wide fog grid is **72 KB** (245×148
cells at 1080p, two bytes each). The engine's own fog grid is 4 KB, the LHT ramp 8 KB, the fog
shade 256 B.

**The wide grid is copied into every packet on purpose, and the alternative was considered and
rejected.** It could be sent only while the render thread is drawing at zoom < 1 — the publisher
knows the commanded zoom, because the command record carries it — but the render thread decides
mid-frame to draw the first zoomed-out frame of a gesture and posts that command at the END of its
frame, so the game thread learns one render frame late. That is exactly the bare frame G13s
removed: one frame of the outer ring drawn over the engine's 1× grid, at the start of every
zoom-out. The copy is what buys "never a bare frame", and it is 72 KB of memcpy on the game thread
per publish.

**THE COST, measured inside ONE launch, because a cross-build one is not available.** Two runs of
`200v200` reach different scenes — the same reason the effects pixel A/B is unavailable — so the
figure that means anything is the same launch with the new gathers turned off and on again. On a
settled 55-unit scene at 1920×1080, `--maxfps 0`, no stress levers:

| phase | publish p50 | p99 | packet | in-play draws/s |
|---|---|---|---|---|
| everything armed | **46 µs** | 234 | 153 712 B | 3389 |
| the four effect tables off | 44 µs | 214 | 147 280 B | 2421 |
| ...and the wide fog grid off | **38 µs** | 224 | 74 760 B | 2374 |
| everything armed again | 44 µs | 230 | 152 912 B | 3540 |

So **the effect tables cost 2 µs of publish and 6.4 KB, and the wide fog grid 6 µs and 72.5 KB** —
8 µs of a 46 µs publish, and 79 KB of a 154 KB packet. The publish's own cost is still dominated by
landing 3's world gather: at ~340 units the same run read p50 112 µs and 308 KB, and at 55 units
46 µs.

**Landing 3's DLL, on a 63-unit scene of the same run's shape, reads p50 30 µs and a 66 392-byte
packet against landing 4's 46 µs and 153 712** — two different battles' leftovers, so it is a
supporting figure and not the measurement; the within-launch phases above are. Turning both new
gathers off inside landing 4's own launch lands at 38 µs and 74 760 B, which is the same place from
the other side.

**The whole build is not slower.** In-play `draws/s` reads 3389 and 3540 on landing 4 against 3347
and 3293 on landing 3, at 55 and 63 units. (The `draws/s` figures for the middle two phases — 2374
and 2421 on landing 4, 1794 and 1850 on landing 3 — are *lower* on **both** builds, because
`fx.on=off` hands the engine back its own projectile, explosion and particle draws and it pays more
for them than our pass does. That is a fact about the effects pass and not about this landing; it
reproduces on the DLL that predates it.) Every one of these is a ratio and nothing else
([ta-drive](../../.claude/skills/ta-drive/SKILL.md), "a frame-rate figure is only ever a RATIO").


### 2.22 What landing 4's review changed

**FIVE Opus reviewers at `high`, read-only, launched as `Agent`s and never `/code-review`** (a fork
runs on the session model). Four read the landing — one per plan row with a numbered risk list, and
one told to range freely and to check the notes' claims against the pristine binary — and a fifth
read the fix diff afterwards. **Fourteen findings between them**, every one verified in the code or
the disassembly before anything moved, and all fourteen acted on except one, rejected below with
its reason. Nine were correctness, five were documentation — and the documentation half is the one
worth noticing, because it included the sentence that licensed a cache.

| # | what it was | why it mattered |
|---|---|---|
| 1 | **both fog answers froze silently** when `terr_fogtick` stopped running | our fog observer runs only while `g_terrown_skip` is set, and the render thread drops that on six documented paths (the `terr.on` lever, `passive`, `over`, a `key=` change, a bail-out, the 90-frame watchdog). `tagpu_fogwide`'s "valid" flag is only ever cleared from inside the tick that has stopped, and the eye latch was never cleared at all — so the packet would have carried the **last grid ever built, for ever**, against a camera and an LOS state that keep moving, with nothing counting it. The wall-clock liveness test landing 4b deleted was never about the hand-over; it was about the producer stopping. The observer stamps the publisher's in-play draw counter now and the publisher accepts either answer only when the stamp is this draw's |
| 2 | **the eye latch outlived its level** | a fresh level can go many draws before the engine's fog builder next fires, so the old level's origin would stand until then. It carries the publisher's level generation too |
| 3 | **the engine's effects draw was suppressed before our tables existed** | the request travels render → game and the fill comes back, so the first frames after `tagpu_fx.on` appears carried empty tables while the pass had already claimed the draw: a handful of frames with no fire, explosions or debris at all. The packet carries `fx_want` and the skip follows it |
| 4 | **a glyph marked sent could be lost for ever**, two ways | the consumer's atlas resets on a shelf overflow and on a ninth font, and the producer's `sent[]` survived it; and an op the consumer DROPS (a surface with no twin, the frames after a reseed) took its glyph records with it. Either way every later string in that font draws with those characters **missing and the rest closed up**. The producer watches the atlas generation, and the drain loop installs the block for every string op before the twin lookup |
| 5 | **the minimap picture rode in exactly one packet** | the mailbox is latest-wins and a dropped packet is a counted statistic — and the likeliest one to be dropped is a level's first, when the render thread is busiest. The render half acknowledges it and the publisher sends until it does |
| 6 | **codes 0x7F..0xFF stopped reaching the glyph cache** | `0x4CCF60` bounds a character below `first` and NOT above, which is why the cache runs to 0xFF; the producer's range was 0x20..0x7E, so a high code drew nothing AND did not advance x |
| 7 | `kind` and `layer` were the only packet indices a consumer formed without a bound at acquire | both are checked in `frame_valid` |
| 8 | **the effects atlas keys on a GAF frame's ADDRESS** and was never invalidated at a level boundary | pre-existing, and finding 11 is what made it matter: a second level's allocator can hand a new frame an old one's address. It drops on the packet's level generation |
| 9 | `tagpu_packet_pub_level_end`'s foreign-thread return skipped the picture reset | the picture's state is keyed on the level generation now and needs no reset at all |
| 10 | **the per-tick cache's stated argument was disproved by the binary** | it said the engine's draw passes read the effect arrays and write nothing. **They do not**: `0x420B00`'s debris loop calls `0x421550` (`0x420B18`), which calls the grey-smoke emitter `0x472810` and the fire emitter `0x472AB0`, and both append to a particle layer. So the layers are not constant within a tick. The cache stands on three weaker things instead, and the comment now says all three: the tables are copies so a later append cannot dangle one; positions are the tick's so nothing already present goes stale; and what it costs is the newest smoke of a tick landing one publish late |
| 11 | **an explosion's two anim states are PER-LEVEL**, not from the session `"fx"` bank | the add site takes the sequence from `main+0x1AB8F[idx]` (`0x420AA2`), a table `0x420620` builds from the level load and `0x420960` frees and nulls from the teardown. The notes said this was "not established"; it is now, and unfavourably — those two frames stand on `tagpu_reclaim`'s fence exactly as the model templates do |

**A FIFTH REVIEWER read the fix diff**, because fixes of that size introduce new mechanisms and a
fix is exactly where a landing stops paying attention. Three of the eleven had added a cross-thread
word. It found **three more, all confirmed**, and they are the reason that pass was worth running:

| # | what it was | why it mattered |
|---|---|---|
| 12 | **the glyph-feed fix was bypassed by the skip-to-reset path** | `drain()` jumps the whole switch for every op queued before a GL context change — its own comment measures 705 of them — so installing the block *inside* the switch still let those ops take their first-sight glyph records with them while the producer had already marked the pairs sent. The block goes in before the skip gate, and the producer's reseed handling clears `sent[]` alongside the sprite and pixel tables it already re-arms. Finding 4's own commit message said "every string op"; it was not |
| 13 | **the fog stamp proved the observer ran, not that the latch was fresh** | after ownership is dropped and returns, the engine rebuilt the grid at a live eye where we could not see it, and on the first tick back `LosType` bit 3 is already set — no rebuild, the stamp matches, and the pre-gap eye goes into the packet against a live grid. Handing the fog site back clears the latch |
| 14 | **the minimap ack could be raised for a picture nobody has** | with `gui.on='mmbase nominimap'` the render half acked without a copy; dropping `nominimap` mid-level then left the level with no picture and nothing to recover it. The ack is withdrawn whenever the copy is absent, which makes every reason it can be absent self-healing |

It also caught the cursor rect's **fourth** outcome, which finding 11's fix had collapsed: a
readable sprite record of zero extent gives the position with a zero size, not "no rect". Making
that exact meant gating on `in_game`, which surfaced the one thing this landing does not close —
**on a shell frame the cursor is the engine's own again**, named above.

**One finding was rejected, and here is why.** The producer publishes glyphs for `0x20..0xFF` and
the reviewer observed that `0x4CCF60` refuses only `0x00` and `0x0A`, so `0x01..0x1F` could in
principle be glyphs too. They could — but `tagpu_text.c`'s cache has always started at `0x20`
(`CH_LO`), so the producer's floor matches the consumer the landing replaced exactly and **nothing
regressed**. Lowering it means widening the consumer's atlas as well, which is a change to a landed
gate for a case no engine UI string contains.

Three more documentation corrections came with them: the particle emitter list undercounted
(**twenty** sites cap a layer, not thirteen — all twenty are listed now, because a partial list
invites the same mistake twice); `f[3]` is the font's FIRST CODE (`0x4CCF77` / `0x4CCFAA`), not the
high byte of the y-offset word, so the `f[3] != 0` test is a refusal of an unusual font rather than
a statement about the format; and `TAGPU_PK_MM_DIMCAP` is a sanity ceiling of ours, the engine
bounding only the BOX it fits the picture into. **The publisher's own comment and its launch log
still said the level generation was reclaim's and "0 for the session" with reclaim off** — the
pre-landing-3 design, changed by landing 3's own review, with the text left behind. A reviewer read
it and reported a defect that is not in the code, which is the cost of a stale comment stated as a
measurement.

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

- **Nothing in this repository has ever been measured on a Windows GL driver, and the first run
  on one found two bugs — 2026-09-10.** `renderer=openglcore` fell back to GDI on a real ICD
  because `glGetIntegerv` was fetched through `wglGetProcAddress`, which returns NULL for the
  OpenGL 1.1 entry points on Windows but not under Wine; and the GL UI layer painted its stale
  twin over the entire intro movie, because the Smacker writes the primary surface directly and
  the publisher never sees it. Both are fixed ([GL UI renderer](gui-renderer.html) §21) — the
  point that survives is the **gap in the harness**: `tacli` runs every instance in a wine
  prefix and skips the intro movies, so neither code path had an oracle at all. The Windows VM
  kit (`_local/vm/`) exists for exactly this and is still waiting on an ISO; until it runs, any
  claim about Windows behaviour in these notes is untested.

- **The feature atlas filled at 48 % occupancy and then rebuilt itself every frame — closed
  2026-09-10 by sorting, not by growing.** `[MEASURED 2026-09-10]` at 3840×2160 / 0.25× on Town
  & Country the pass reported `atlas=204 DROPPED(atlas-fail=455)`, a different 3.7 % of the
  feature quads missing each frame, and the atlas latched `full` at **48 %** of its 2048 square.
  The shelf packer is fed in map order, so a 320-tall tree opened a shelf that a row of 12-tall
  rocks then sat in: 197 of the map's 229 feature frames placed, 86 % of the page consumed, 52 %
  of that air. `tagpu_feat_gather` then reset the atlas whole on the next frame, restoring the
  same order into the same geometry — **37,140 resets in one 4K session**, each re-decoding ~200
  frames from RLE, re-uploading them to build a byte-identical layout, and clearing the
  Classic++ restore queue (`tagpu_rglsl_job_clear`, which clears the twin), so the feature twin
  could never converge while zoomed out. `tagpu_gaf.c`'s `atlas_repack` now re-lays the entries
  **tallest cell first** — the reset was always the one moment the packer had perfect
  information, and `atlas_reset` never cleared `ents` — and *reserves* the rects rather than
  filling them, because we keep no decoded pixels and GL 3.3 core has no `glCopyImageSubData`;
  each frame re-decodes into its new rect on its next `atlas_get`. Re-measured on the same
  scenario, resolution and zoom: **`atlas=234`, no `DROPPED` line at all, 0 resets, 1 repack**
  (`205 frames re-laid tallest-first, 50% of the 2048 square, generation 4`), every other field
  on the pass's line unchanged. Confirmed visually at 4K on the reference setup's real GL.
  `repackWall` latches when a repack cannot place everything still being asked for, and the atlas
  then *holds* its layout, naming a second page as the only thing left that adds room — nothing
  has reached it (42 % of the page is still untouched after the repack, and of the four atlases
  only this one has ever filled). **The landing review caught that the first version made that
  wall terminal**: `full` stays latched behind it and the only thing that cleared it was
  `tagpu_gaf_atlas_lost`, reached solely from `tagpu_native_glreset` on a display-mode change, so
  a session that walled on one map would have drawn no features at all on the next. Two
  independent fixes: `tagpu_feat_gather` keeps the `FeatureMap` pointer (`main+0x14287`) and the
  map's 16-px dimensions and calls the new `tagpu_gaf_atlas_forget` when any moves (the identity
  test `tagpu_terr.c` makes on its `TILE_SET`), and a repack now re-lays only entries something
  has asked for since the last one — so a map change the pointer test missed still cannot
  accumulate. See [features](features.html) §5 and [the GAF sprite atlas](atlas-packing.html) for
  the branch, the multipage cost, and the residual it leaves.
- **A crash in the Classic++ shadow pass, fixed 2026-09-09 — and the fix is a BOUND, not a
  probe.** `[MEASURED 2026-09-08]` a 400-unit game under the play defaults faulted at
  `fild [ebx+0x10]` inside `tagpu_native.c`'s `aabb_walk`, `EBX = 0x3D1E4B1E`, seven levels into
  the recursion, ~46 s in — `C0000005`, no warning, and identified only by matching the
  `ErrorLog`'s bytes at EIP against our own DLL (the report names `TotalA.exe` whatever module
  faulted; the IP was our relocated base plus `0x48462`). The caster loop took the unit record's
  `ModelId` (`unit+0xA6`) straight into the model-template table with no bound at all, so a unit
  slot recycled between the frame's gather and the shadow loop addressed memory past the end of
  that table and the pointer read from there was walked as if it were a model. Now it goes
  through `model_root`, which holds the index to `1 <= mid < UNITINFOCount` — the exact range the
  engine's own load and free loops use, so an in-range slot holds NULL or a live template by
  construction ([engine map](exe-reverse-engineering.html) §`0x42D5xx`). The walk itself gained
  **no** per-node `IsBadReadPtr`: a readability probe is a fact about the past and would have
  made the fault rarer rather than impossible (`CLAUDE.md`, *Fixes must be safe by construction*;
  [thread-safe destruction](thread-safe-destruction.html) §2 on what a Mode B read owes). The
  selection-rect loop read the same table the same way and now shares the bound. Re-measured: the
  case that crashed ran 7.5 minutes of `200v200` plus a quit to the shell and a second level, no
  fault, `BADMODELID=0` — the counter the `native:` line grows only when the bound catches
  something, which so far it never has.
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
| **Classic++ restored art is not exactly linear in the Gamma factor.** The restorer expands the indexed atlas through the palette the screen is shown with (§2.3f), so at a factor other than 1.0 the model sees brighter art than it was trained on. MEASURED 2026-09-09, Two Continents at factor 1.5 against `min(255, the factor-1.0 frame × 1.5)`: 88 % of the viewport differs, but by **more than 6 levels on 0.68 % of it**, max 19. The Classic (indexed) path is exact at every factor; this is the Classic++ lane only | `tagpu_pal.c`, §2.3f | restoring in the unscaled domain and applying the factor where each twin is *sampled* — five shaders in place of one palette, and it would also retire the repaint |
| **A Gamma change mid-game costs a full Classic++ re-restore** — 2.5 s of sliced GPU work per atlas that has one, the terrain's being the large one. Bounded (one repaint in flight per atlas) and progressive (no blanking), but it is real work for a slider the player is dragging | `tagpu_gaf.c` / `tagpu_terr.c`, §2.3f | nothing planned; the same "sample-time factor" change above would remove the need entirely |
| ~~**`tagpu_fog_at`'s dimension bound is 512 and the producer's cap is 1024**~~ **CLOSED 2026-09-10.** The gate now bounds against `tagpu_fogwide_dimcap()`, published by the code that does the allocating and a high-water mark so it can only ever be too generous; the engine-descriptor bound in `tagpu_native.c` became the shared `FOGW_ENGINE_DIMCAP`. The three numbers that had to agree are one. The grid itself is no longer a fixed square either — it is sized from the window the screen asks for at the worst eye residue (`fogw_capacity`), **212 KB for the set at 1920×1080 and 84 KB at 1024×768 against the old 6144 KB in every session**, and the set is grown behind `tagpu_reclaim`'s quiescence fence because the render thread holds `s_hold` across a whole frame with no lock | `tagpu_fx.c` `tagpu_fog_at`; `tagpu_fogwide.c` `fogw_capacity`/`fogw_alloc`/`fogw_retire` | the grow path has never run from a real video-mode change — only from a temporary probe. A session that goes game → shell → game at a different resolution is the untested path |
| **The unit array's `beg`/`end` pair is read across the thread boundary as two unsynchronised loads** and then used as the bounds of a `+= 0x118` walk that dereferences every slot. **Audited 2026-09-11** ([cross-thread engine reads](cross-thread-engine-reads.md) §5): of the nine files the sweep counted, six read it on the render thread below the teardown gate (`tagpu_native.c`, `tagpu_scaffold.c`, `tagpu_mark.c`, and `tagpu_overlay.c`'s `log_units`/`probe_unit_model`/`writeback_paint`), three read it on the game thread (`tagpu_order.c`, `tagpu_scenario.c`'s resolvers, `tagpu_weapons.c`), and two read it on the render thread *before* the gate but only on a `tacli` trigger (`tagpu_cat.c`, `tagpu_tracer.c`). The fence covers the teardown — `0x485980` frees the array and nulls `begin` inside the cascade — but **not the next level's load**: `0x4854A0` stores `begin` (`0x485525`), memsets the whole array, makes two more allocations, and only then stores `end` (`0x4855D6`), which is the only store to `main+0x1435B` in the binary, so `end` is never nulled. For the length of that memset the pair is (new `begin`, last game's `end`): the same block and nothing shows, a lower block and the walk runs past the new allocation into freed memory. Alignment plays no part in it; a torn load is a rarer variant of the same outcome | the six render-thread `OFF_BEGIN` reads, `tagpu_native.c`'s first | **`(end - beg) % 0x118 == 0` is a filter, not a fix** — random skew passes it 1 time in 280 — and is NOT the check `tagpu_order.c:753` / `tagpu_native.c:3938` already make (`(u - beg) % UNIT_STRIDE` on a *candidate pointer*). The **exact** relation `end == beg + (count-1)*0x118`, with the slot count the engine publishes at `main+0x14351` *before* `begin`, refuses every skew that is not benign and is worth adding as a refusal layer; against a torn load it is still only a filter. The by-construction answer is the fence `tagpu_reclaim` already applies to `0x491B60`, applied to `0x4854A0`: hold the reader off for the length of the allocation, or publish the pair from its post hook into aligned fork statics. A change here is a `high` review |
| **`tagpu_overlay_draw`'s two early returns above the teardown gate still run `writeback_paint`**, which dereferences `*(char**)(u + U_OBJ3DO)` — after `tagpu_reclaim_pass_begin` has already published `s_completed = s_started` for that pass. The gate's own comment says the writeback must not run during a teardown, and those two paths sit above it. Found by the G13t re-review while checking the latch that closed the same hole one line lower | `tagpu_overlay.c` lines 583 and 585, `writeback_paint` | reachable only with `tagpu_writeback.on` **and** (`tagpu_overlay.off` or `s_state != 1`), so it is debug-lever-only and a no-op in play — which is why G13t recorded it instead of widening its own diff. The fix is the same shape: those returns must consult the latched flag too |
| **`tagpu_reclaim.c` claims every reader of the UnitDef array is on the game thread**, and uses that to justify leaving the free at `0x42DCCB` unhooked. But `tagpu_cat_frame`, `tagpu_weapons_frame` and `tagpu_scenario_frame` all run from `tagpu_overlay_draw`, on the RENDER thread, above the gate. PLAUSIBLE rather than confirmed — nobody has reproduced a fault — but the stated reason for not hooking it does not hold | `tagpu_reclaim.c` ~line 215 and the three `*_frame` call sites in `tagpu_overlay.c` | all three are trigger-file gated, so a no-op in play. Either hook the free or correct the justification; do not leave the justification standing |
| **Past about 7680×4320 the wide fog grid is clamped and the outer ring smears again.** `tagpu_fogwide`'s three buffers are `FOGW_MAXDIM` square and allocated ONCE — it publishes a pointer into `s_pub` to the render thread while the game thread builds into `s_build`, so a buffer grown under a zoom change would be a use-after-free — and 1024 cells covers the window a real screen asks for (485 at 3840×2160, 645 at 5120×2880, 965 at 7680×4320, all MEASURED against the arithmetic 2026-09-09). Past that the clamp takes its trim off both ends, so the view's centre keeps its cover and only the edge returns to the border-cell smear | `tagpu_fogwide.c` `FOGW_MAXDIM` | a bigger allocation, or a publish handshake that makes growing one safe; neither is worth it for a screen nobody has |
| The **unit** pass's `MAXU`/`MAXNV` are the first fixed budgets a very wide zoomed-out view meets, now that the terrain's and the feature pass's are the screen | `tagpu_native.c` | measure how many units a 4K 0.25× view over a full map actually gathers, then size or bail deliberately. The feature pass's `MAXBV_BODY`/`MAXBV_SHAD` were this row's other half until 2026-09-10; they are gone — `tagpu_feat.c`'s buckets `realloc`-double from `BV_BODY_0`/`BV_SHAD_0` behind `feat_room()` up to a 16 MB ceiling, and a 4K 0.25× view on Town & Country grew them to 65536/32768 verts with `DROPPED(full=0)` |

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
