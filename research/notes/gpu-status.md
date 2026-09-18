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
- **Ray tracing is out of reach while the renderer is in-process** [MEASURED 2026-09-15].
  Not a limit of our code: NVIDIA's 32-bit ICD does not advertise
  `VK_KHR_acceleration_structure` / `ray_tracing_pipeline` / `ray_query` /
  `deferred_host_operations` at all, and our renderer is a 32-bit DLL inside `TotalA.exe`. The
  same probe in a 64-bit process on the same card and driver has all four. Bitness is the only
  variable that moves it, across two wine versions, with the llvmpipe software device as the
  control. Choosing *which* GPU renders is NOT affected — 32-bit Vulkan enumerates both devices
  and flags the discrete one — so that stays reachable from the DLL as it stands. The table, the
  control and what it does not establish: [field notes](field-notes.html), "Environment &
  toolchain"; the probe is `tools/vkprobe.c`.
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
| `0x459338` | `call 0x45A470` in the blit `0x459200`, path A — the completed-unit **silhouette shadow**: `0x45A470(this, composite)` fills the scratch with the unit's own composite blackened (every non-ColorKey texel → index 0, `0x4B96A0`) and the ALP blend `0x4B8500` writes it at `sx+0x85`. Inside `terrown`'s key-filled viewport its destination is palette 254, so a shadow built from a NON-empty composite lands as an OPAQUE TEAL `(0,128,128)` silhouette on the unit. Measured 2026-09-13: at map entry the commander keeps one until it first moves ([exe-reverse-engineering](exe-reverse-engineering.html) §"The completed-unit shadow") | `owndraw` | 5-stolen call-site detour, bytes matched first; the stub replays the call and empties the composite first when the unit is ours AND `tagpu_posedraw_live()` — and a husk is not ours unless `tagpu_native_wrecks_armed()`, the classifier's own first branch (defensive: on the `one-wreck` fixture with `native.on` = `all` and no `wrecks` token, disabling the clause changed nothing — the unit predicate does not answer yes to a husk there) |
| `0x45958C` `0x4594DB` | the same three instructions in path B (colour+depth) and in its inline digger branch | `owndraw` | installed with the site above as a set of three or not at all |
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

**What the landing review found, and what it settled** (two reviewers, `high`,
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

**The options/exit/preferences stack owns the point before either path can treat it as world space.**
F2 and Tab set `main+0x37EBE` bit 0 for `ARMOPT.GUI`; it stays set through
`EXITMENU.GUI`, `YESORNO.GUI` and the preferences screens while `DrawGameScreen` keeps
publishing the zoomed world underneath. The shared `to_engine()` door therefore reads the bit
before its geometric viewport test and leaves the point unchanged while it is set. This covers
hardware messages, injected button messages and the mouse→world repair with one read-only
gate; the DLL-owned render-options panel remains the separate `tagpu_menu_owns_point()` case
because it sets no engine ownership bit. Measured at 0.25× and 8×: `EXIT`, `MAINMENU`,
`EXITGAME`, both confirmation choices and preferences `OK` landed on their reported gadgets;
Resume cleared the bit and restored world input.
This is deliberately narrower than "a built-in in-game GUI": `SHARE.GUI` sets bit 6 at
`0x49374F`, not bit 0, and remains a known gap under zoom.

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
| `0x4C67C0` | the cursor draw **inside** the flip (`stdcall(globals, surface)`, `ret 8`), the shell's publish point since landing 6 — **its own observer, in `tagpu_packet_pub.c`**, not this census. The function itself is UNPATCHED: since 2026-09-14 `tagpu_cursown.c` skips only the `call` at `0x4C687D` that blits the sprite, so the observer, the background save at `0x4C6862` and the `+0x1B6/+0x1BA` writes all still run (a leaf on the whole function was tried on 2026-09-13 and withdrawn — GUI renderer §24.0) | 11 | the drawn cursor: `+0x1B2` the record, `+0x1B6/+0x1BA` the position it just wrote. Publishes a header-only `in_game = 0` packet with `cursor_live = 1` when the three early-out words hold, and `cursor_live = 0` when they do not — gated on **two** tests, `s_retDepth == 0` **and** `!s_levelOpen`. `s_retDepth == 0` alone is NOT the complement of the in-play gate: `0x495E66` calls `DrawGameScreen` and returns to `0x495E6B`, not the in-play `0x4969D2`, so a screenshot draw is in-play with `s_retDepth == 0`; `!s_levelOpen` is what keeps this channel out of a level. This row said "i.e. not inside an in-play draw" until the 2026-09-14 review — the code has always had both tests |
| `0x4C25E0` | the body of the engine's **mouse thread** (`0x4C2990` is its entry, started by `_beginthread` at `0x4C2A9A` — which is why no `call 0x4C2990` exists); `stdcall(mouseObj)`, `ret 4`. Unpatched as a function; its cursor blit `0x4C2732` is one of the four `tagpu_cursown.c` skips | 8 | nothing — no observer, it is not a channel site. It writes `+0x196/+0x19A` and `+0x1B6/+0x1BA`, the latter as position **minus the hotspot**, exactly as `0x4C67C0` does [CORRECTED 2026-09-14: this row claimed the opposite and called it a fingerprint] |
| `0x4B7F90` | `CopyGafToContext(ctx, frame, x, y)` — **chained onto fxown's stub** | 6 | a sprite box at `(x−HotX, y−HotY)`, clipped |
| `0x4B8500`, `0x4B8310` | the shaded blit and DrawText's alternate blit, same shape | 6 | same |
| `0x4C6D20` | descriptor blit `(ctx, desc, src, dst)` — listbox, textfield | 7 | `*dst` |
| `0x4C7580` | the textured-triangle stamp `(ctx, src, xy[6], uv[6])` — the option screens' wide backdrop | 5 | the vertices' bounding box |
| `0x4CCF60` | the glyph blitter, cdecl 9 args | 6 | the string's box from the font's width table, the string's bytes copied into a window scratch, and **since G19f-8 the font itself**: the slot id, `font[0]`/`font[2]`, and a glyph record for every code of this string the font has not sent yet (`text_capture`). `publish` then dereferences no font at all — the read happens one instruction before the engine's own, which is the whole of the lifetime argument |
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
| `main+0x37EBE` bit 0 | options/exit/preferences-stack ownership. **Read only**, at the shared screen→world input transform: set across `ARMOPT.GUI`, `EXITMENU.GUI`, `YESORNO.GUI` and preferences, even though `DrawGameScreen` continues underneath; while set, the whole transform is the identity so those gadget clicks stay in 1:1 screen space at every zoom (§2.3d). Measured Resume/close paths clear it before world input resumes. **Not a general modal flag:** `SHARE.GUI` sets bit 6 and is not covered |
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
| **the unit composite's planes** — `Object3do+0x10` → the GAFFrame's colour plane (`+0x10`) and depth plane (`+0x14`), `w×h` bytes each. **WRITTEN — the planes are overwritten with the ColorKey (index 1) and far depth, on the GAME THREAD, in two places.** `tagpu_owndraw_classify` does it when it skips the engine's rasterise for a unit `tagpu_native_owns_obj` accepts (G12b), and since 2026-09-13 `tagpu_owndraw_preshadow` does it again at the completed-unit shadow's own emit sites (`0x459338` / `0x45958C` / `0x4594DB`), before the replayed `0x45A470` copies the plane into the scratch — because the classifier's wipe does not survive to the shadow on every frame (none of 7047 wipes in a 60-frame window found the plane already empty; [roadmap](roadmap.html), "G13l follow-on"). A render-side scratch the engine rebuilds from the posed prims, not sim state: the blit and the shadow read it, nothing else does, and every value taken out of it is bounded by the unit walking |

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
| `tagpu_ghost.on` | | `native` |
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

**Two uniforms this program does NOT hold what `tagpu_native.c`'s holds** [FOUND 2026-09-15 by the
Vulkan port, which had to reproduce them]. Both are facts about the shipped GL renderer, recorded
because a port that "fixed" either would draw a different picture from the game:

| | |
|---|---|
| `uLambert` | **never set at all.** `tagpu_posedraw.c` does not look the uniform up and never writes it, so it keeps a freshly linked program's **0** for the pass's life — and the Classic++ lambert therefore lights a unit from a **flat up normal** (`vec3(0,1,0)`) rather than from `vNrm`. `tagpu_native.c:4010` sets it from `tagpu_classicpp_lit()` on ITS program. Whether that is deliberate has not been established; what is established is that it is what the game draws |
| `uRestored` | set from `tagpu_r3d_atlas_rgbref() && tagpu_classicpp_ON()` here, and from `… && tagpu_classicpp_ASSETS()` on the native program. The two disagree whenever the master arm is on and `assets=0` |

A third is not an asymmetry but is the same kind of trap: **`uNanoT` and `uNanoC` are STICKY**. The
pass writes them only on a unit with `nanoOn`, so a unit without one is drawn against whatever the
last unit that had one left in the program. Nothing reads them on a `uNanoOn == 0` unit, which is
what makes that safe — and it is a statement about the fragment shader, not about the upload.

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

**The front-end screen carries sixteen rows in two columns** [2026-09-11; the GPU row added
2026-09-15]. `VISUALS.GUI` is re-emitted into the same `.ufo` with the stock eleven gadgets
moved (names, `assoc`, `commonattribs`, `range` and `stages` all verbatim) and five rows of our
own added:

| column | rows |
|---|---|
| **Window** | Display mode (window / borderless fullscreen, `util_toggle_fullscreen`), Monitor (`EnumDisplayMonitors`, `SetWindowPos`), UI scale (Auto / 1x..4x, the client set to k x the Screen Size row's own mode at `main+0x37F1B/+0x37F1F`), Screen Size (stock `VIDSLDR`), Frame cap (60 / 120 / uncapped, `g_config.maxfps` + `fpsl_init`), Gamma (stock), **GPU (Vulkan)** (G19b — `tagpu_vk.h`; caption at y 364, control at 380, in the space the Gamma slider left free) |
| **Impure rendering** | Renderer, Undithered assets, Dynamic lighting, Shadows, Shadow quality, Shading (stock), Anti-aliasing (stock), Engine shadows (stock `BSHADOWS`), Supersampling |

Four things this rests on, each measured rather than assumed:

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
- **The GPU row's list is one launch behind, by construction** [G19b, 2026-09-15]. Its captions
  have to be inside this generated `.GUI`, and `tagpu_menu_init` writes the archive at DLL
  attach — where creating a Vulkan instance (and so loading an ICD) is exactly the
  `LoadLibrary`-from-`DllMain` [field notes](field-notes.html) forbids. So a worker enumerates
  the devices once the render thread is up and writes `tagpu_vk.gpus`, and the menu reads that
  cache at the *next* attach. It is the same bargain the Monitor row already makes for a
  hot-plugged monitor. The row's **model** is not one launch behind: the choice is stored by
  name in `tagpu_vk.cfg`, and `read_display_state` plates `tagpu_vk_gpu_active()` — the device
  the render thread actually bound — whenever the lane is up, so a request that could not be
  honoured shows as the device that was.

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

**Since G19d it is also the FIRST pass with a Vulkan edition** ([§2.26](#226-the-fps-readout-drawn-by-vulkan-tagpu_vk_fpsc-tagpu_vk_passh-tagpu_vk_shotc-phase-g-g19d)),
and two things about this file changed for it. `tagpu_fps_quads` hands the frame's vertices over
**exactly once** — the freshness rule, since the overlay does not run on every path that reaches
the swap and a frame it skipped must not redraw the last one's text. And `tagpu_fps.ab` captures
one frame of the readout over a black field to `tagpu_fps_gl.ppm`: an **oracle** rather than
instrumentation, because a readout drawn over the game cannot be compared against anything with
the game under it. It clears the whole frame on purpose, once, until the lever is taken away.

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

**What the landing review changed (two reviewers, `high`, 2026-09-12; both could construct no
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

**What the landing review changed.** Two reviewers at `high` over the branch diff, one
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
| the same `after` | **the gather is taken once per SIM TICK**, keyed on `(level generation, tick)`. Measured on a live `200v200` at 1080p: **5 592 scans against 80 469 reuses, one gather in 15 publishes**, at 634 publishes a second against a 60 Hz sim. The LEVEL half of the key is what stops a hit across a boundary handing the native pass a model root the teardown has freed. **The argument is NOT the anchor scan's** — that grid really is constant within a tick because only the tick writes it, and these four are not: the engine's own explosion DRAW emits particles (`0x420B00` → `0x421550` at `0x420B18`, which calls the grey-smoke emitter `0x472810` and the fire emitter `0x472AB0`, both of which append to a layer). What licenses the cache is weaker: the tables are COPIES, so a later append cannot dangle one; positions are the TICK's, so nothing already present goes stale; and what it costs is the newest smoke or fire of a tick landing one publish late. [The sentence this row carried — "the engine's own draw passes read them and write nothing" — was disproved by landing 4a's review; §2.22 finding 10.] | game |
| `tagpu_fx.c` | the three effect tables and the engine's DRAWING RULES over them — which rendertype makes which primitive, where the shadow blob goes, how the lightning bolt jitters. It reads no engine memory at all and is **off the allow-list**; the model roots it carries are per-TYPE templates it never dereferences, passed straight to `emit_fx_model` in the (fenced) native pass | render |
| `tagpu_sfx.c` | the particle table, walked by layer because the layer IS the draw depth (0..6 before the projectiles, 7..9 after the explosions). Also **off the allow-list** | render |
| `tagpu_gaf.c` | gained `tagpu_gaf_frame_geom` / `_subframe`, so a pass that only PLACES a sprite dereferences no engine byte of its own; the publisher calls its resolvers on the game thread | both |
| `tagpu_fxown.c` | the render thread's standing request for the tables (`want`), with the same 90-frame watchdog the two skip bytes stand on: an unarmed pass costs the publisher nothing | both |
| `tagpu_native.c` | **`g_wantBuilds`** [2026-09-14] — the BUILD GHOST's standing request for the packet's builds table, the same pattern: written by the render thread from the ghost's own 30-frame poll (`tagpu_native_set_want_builds`), read by the game thread in `fill_builds`, which otherwise walks the order arena and copies up to `TAGPU_PK_MAX_BUILDS * 16` B into the packet every frame for a pass that may not exist. Its 90-frame watchdog is `tagpu_native_flush_want`, driven from `tagpu_overlay.c`'s unconditional flush run — deliberately NOT from the setter, where the first version put it and where it could not see the one case it was for (a render thread that stops calling: a refused driver returns in front of the poll). The table is therefore CONDITIONAL; `tagpu_pk_builds()` returns NULL at `n_builds == 0` and every consumer loops to `n_builds`, so an absent table reads as an empty one | both |

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

**The 120-stop `strict` UI walk, and the one column that moved.** `tools/uiwalk.py --inst <i>
--res 1024x768 --layer --cycles 2`, run on landing 4's build and then, identically, on landing 3's
— with `classicpp.on=off`, which is not optional: the walk diffs our GL frame against the engine's
**indexed** surface, and the restorer makes the two halves come off different art. **Seventy stops compared
column by column, and every column that is a measurement reads the same on both builds**:

- `hit misses` and `drift px` **identical at every stop**, the one standing `MISS=1 [FPS]
  drift=23px` on `VISUALS.GUI` included — it reproduces on landing 3's DLL at all three of its
  cycles, so it is `uiwalk`'s hit-test inverse on a row `tagpu_menu.c` emits (*The front-end screen
  carries fifteen rows in two columns*, above) and not this landing's.
- `differing px outside the viewport` **0 at every stop** but the four `MAINMENU.GUI` ones, which
  the [ta-drive](../../.claude/skills/ta-drive/SKILL.md) skill already records as that screen's own
  sparkle animation between the two shots: 181–184 here against 179–188 on the reference.
- `inside the viewport, engine non-key px` **0 / N at every stop** but `space-popup`, and that
  stop is unstable on both builds rather than different between them: 41 of 37 294 here against 22
  there, in the same 6x8 box at the bottom of the viewport, with the engine's OWN between-shot
  self-difference reading 15 here and 93 there. The popup is mid-animation in both, and the
  engine's two captures do not agree with each other either — it is a stop to re-take, not a
  number to compare.
- The two 8-column census tables are **identical**: `censuses`, `changed px`, `unexplained px` and
  `ops in the window` match at all seventy stops.

**The one column that moved is `strict holes`: 4 on landing 4's DLL at two in-game stops (`F4@0.5`
and `move`), 0 on landing 3's** — and the cause is worth the space, because it is a property of
reading the cursor from the packet and not a mistake in the conversion.

The engine's cursors PULSE. The `curs=` counter reports the move cursor at 27×27, 29×29, 31×31,
33×33 and 35×35 on **both** builds — one pixel per side per animation step. The rect the layer
exempts from the fallback and from `strict` used to be read live on the render thread; it is the
packet's now, taken inside `DrawGameScreen`, and the engine blits its cursor onto the primary
*after* that. So the surface the layer composites against can already hold the NEXT step, and where
the next step is larger its outermost ring is neither drawn by our sharp layer nor exempt. On this
art — a four-way arrow whose outermost ring is four isolated tips — that ring is exactly **four
pixels**, at (496,384), (512,368), (512,400) and (528,384) around a pointer parked at the screen
centre, with the engine's two surface captures **byte-identical between the builds** (0 px) at that
stop. It is one animation step, at two of forty-five in-game stops.

**What a player sees is not magenta.** `strict` is the harness's mode; with the fallback on, those
four pixels show the engine's own cursor art, so the frame carries the union of two adjacent steps
of the same sprite — a cursor that looks one step thicker for one frame. **The fix is a bound, not
a margin**: the exempt rect has to cover the sprite the engine will blit *next*, which means
publishing the cursor's animation extent out of the cursor table — per-process and immutable, so a
constant once read — and nobody has read that table's shape yet. Padding the rect by a guess is the
timing argument `CLAUDE.md` refuses, so it is named in
[GUI renderer](gui-renderer.html) §23 *Not closed here* and left for the landing that publishes it.

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

**FIVE reviewers at `high`, read-only, launched as `Agent`s and never `/code-review`** (a fork
runs on the session model). Four read the landing — one per plan row with a numbered risk list, and
one told to range freely and to check the notes' claims against the pristine binary — and a fifth
read the fix diff afterwards. **Fourteen findings between them**, every one verified in the code or
the disassembly before anything moved and every one acted on — plus a fifteenth, rejected below
with its reason, and three further documentation corrections. Twelve of the fourteen were
correctness and two were documentation, and it is the documentation pair worth noticing, because
one of them is the sentence that licensed a cache.

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
that exact meant gating on `in_game`, which surfaced the one thing this landing did not close —
**on a shell frame the cursor is the engine's own again**, named above. **Closed 2026-09-13 by
landing 6** (`tagpu_packet_pub.c`, the shell cursor channel, observed on the flip's cursor draw
`0x4C67C0`): the gate is `in_game || cursor_live` now, and the shell publishes its own
header-only packet. Measured on the main menu: `curs=1,10x20,…`, and the cursor's zone is
**0 px different** between the layer on and the layer off.

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

### 2.23 The build ghost (`tagpu_native.c`, a play default since 2026-09-14, `tagpu_ghost.on`) — 2026-09-12

A translucent copy of the building under the placement cursor and of every queued build the order
pass is showing a site rect for, drawn through the posed program in the **model's own colours**
at the ghost's alpha, over the footprint squares exactly as the mark and order passes draw them.
The squares alone carry the green/blocked distinction; the ghost reads no colour at all. Nothing
about the squares changes; no new engine hook and no engine write: the ghost is a posed body draw
whose DATA arrives entirely in the frame packet.

- **The data.** `TAGPU_PK_BUILD`, a build-orders table the publisher copies out of the order
  pass's own game-thread snapshot — the same records `draw_build` draws the squares from, under
  the same gate (`s_build`, a non-zero `btype`, a resolved def) and the same lever arm state, so
  the ghost and the squares can differ only by the copy's age: the squares read the arena at
  present time, the ghost the copy the previous draw made of it, one presented frame at most.
  The cursor's unit type is `build_unit_id`, the header's copy of `BuildUnitID main+0x2CC4`
  ([VERIFIED LIVE](exe-reverse-engineering.html): 78 = ARMMEX after a build-menu click), with the
  cursor's position the midpoint of `build_rect` and its gate `gather_cursor`'s own (mode 14, and
  the band bit or the mouse inside the rect the engine can NAME). Both ghosts bound their type by
  `udef_count` before the model table.
- **The draw.** The ghost pass lives in `tagpu_native.c` (the `fenced` file whose allowance the
  `MODEL_PTRS` template read stands on) and synthesises the piece list from the model tree —
  parents first, at rest, the same walk class the bake and `PK_PIECE.node` stand on — then bakes
  and poses: a **rest pose**, per-piece translation by the bake's `restOff` (posed_pose's own
  output for a unit holding every piece at rest), into render-thread scratch. One ghost = one
  bake lookup, one `glDrawArrays`, no pose arena. `fog=0` so it never fog-dims, like the square;
  no shadow, no nanoframe wire, no waterline. The material is owner 0, so the ghost shows the
  player's own team colour — exactly what the built unit will look like. An earlier cut tinted
  it green/red (`uGhostTint`); the owner dropped the tint (2026-09-12): translucency alone, the
  existing `uAlpha` blend the cloak already rides.
- **The cache key — and the leak it closed.** The synthesised run walks the tree in *its* order,
  which is not the prim order a live unit's packet run carries, and the bake lays the VBO's
  per-vertex piece indices and `parent[]` out in run order. So the ghost's bake is keyed apart
  from the units' (`TAGPU_PBGEOM.ghost`, a `ghost` parameter on `tagpu_posebake_unit`): the first
  cut shared the slot, and a placed building — same root, same piece count — then drew its
  prim-ordered pose matrices against the ghost-ordered VBO, scrambling every part ("all of the
  model parts of building are translated in an incorrect manner"; the commander was immune
  because it baked before any ghost existed). Safe by construction: the two runs can never share
  an entry whatever the walk order.
- **The shader.** None. The ghost rides the posed program untouched — `uAlpha` is the cloak's
  blend, and the model's own colours pass through; the only per-unit field it adds is the
  ordinary `alpha`. (The 2026-09-12 tint cut removed `uGhost`/`uGhostTint` again, so
  `tagpu_native_unit_fs` is back to the shape it had before the feature.)
- **The piece walk's provenance, now that the ghost is a default.** `ghost_pieces` carries no
  per-node readability probe, on `aabb_walk`'s argument and by its lifetime (the level, under
  `tagpu_reclaim`'s teardown wrap). One difference from `aabb_walk` is worth stating rather than
  leaving to be found: `aabb_walk` walks the template of an INSTANTIATED unit, while
  `ghost_pieces` walks `mptrs[mid]` for any `mid` the caller's `mid < udef_count` bound admits —
  a def the player can select to build but whose model slot this level may never have
  instantiated. The slot is guaranteed non-NULL by that bound; that it is *walkable* is inferred
  from the engine's own unconditional walk at `0x4CB650`, not measured for a never-instantiated
  def. [Named by the 2026-09-14 landing review; no fault observed in 57 480 walks.]
- **The lever.** `tagpu_ghost.on` (tokens: `alpha=<f>`, default 0.40), re-read on the pass's own
  30-frame poll; the armed line and the `ghost: curs= queue= drawn= nobake= trunc= alpha=`
  heartbeat log only on change / every 300 frames. `nobake` and `trunc` must stay 0. **It needs
  `tagpu_native.on`** — the ghost draws through the unit pass's view and program — and says so:
  armed without it the log reads `ghost: off — needs tagpu_native.on (it draws through the unit
  pass)` and the pass declines. **Since 2026-09-14 it IS a play default** and carries `needs
  tagpu_native.on` in `tagpu_opt.c`'s table, so the table withholds the default when the unit
  pass is off. The runtime refusal above stays, and is not redundant: `needs` governs the
  DEFAULT, not the lever, so a hand-written `tagpu_ghost.on` file arms the pass whatever the
  table says. Turn it off with `tagpu_ghost.off`, or with `tagpu_defaults.off` for the whole
  table.
- **The review's fourteen findings (2026-09-12, xhigh, one reviewer) — what changed.** The pass
  now checks its two prerequisites every frame (the posed program live; `s_pv` THIS frame's,
  stamped when the unit pass fills it) instead of assuming them; it re-binds the posed program's
  seven texture units itself (`ghost_bind_textures`) because `tagpu_fx_render` rebinds 0/1/2/4/5/6
  for its own program and restores nothing; it draws with **depth writes off** so two ghosts at
  one site blend instead of the second being culled by the first's depth; it culls both ghost
  kinds against the widest-zoom rect the unit gather uses; the cursor's `build_rect` is bounded
  and projected exactly as `gather_cursor` bounds it (so a rect the square refuses draws no
  ghost, and the midpoint adds cannot overflow); the anchor halves the altitude with the
  **square's** truncating `>> 1`, not the unit pass's float, so the ghost sits ON its square; a
  model the piece walk cannot hold is **refused and counted** (`trunc`) rather than drawn as half
  a building; the queue table is validated like every other packet table (`frame_valid`); and
  `tagpu_order_copy_builds` is gated on the order pass's own arm state, so disarming it takes the
  queue ghosts with the squares instead of leaving the last publication frozen on screen.
  **Measured in game**: the cursor ghost centred on its square (bbox 975..1019 against square
  973..1021, centroid x 996 vs 997); queue ghosts drawing from a real record (`bt=78 bdef=1`
  through the packet); `posed=2/361tri` with **no `q=` suffix** while thousands of ghosts drew
  (the false "queued units not drawn" heartbeat this fixes); `trunc=0`.
  **Not verified in game**: the depth-overlap and disarm checks — the cursor and queue ghosts are
  not co-located (their anchors come from `build_rect` and the order node, ~18 px apart on the
  tested site) and every candidate site was animating (a nanoframe under construction), which is
  a noise floor of ~2.8 k px against a ~950 px ghost. Both changes are read-verified against the
  code path they guard.
- **Latent, not fixed by the rebind alone.** With Classic++ on (the play default) a body fragment
  takes its colour from the rgb atlas on **unit 8**, which fx never touches, and samples unit 0
  only for the texture's colour-key discard; measured this session, unbinding units 0/1/2 changes
  the ghost not at all in that configuration. The clobbering bites when the LUT/palette path is
  live (`uLit == 0`, Classic++ off) and, for keyed textures, in the discard test — which is what
  the rebind removes.
- **Verified in game** (one-unit fixture, play defaults): the mex ghost at the placement cursor
  and at the queued site rects, diffed against `ghost.on=off` and found only at the ghost's own
  footprint; after the cache-key fix, a placed mex renders pixel-identical with the ghost armed
  and disarmed (mean diff 6.0 vs the 6.7 off/off baseline).

### 2.24 The Vulkan lane (`tagpu_vk.c`, **off unless armed**, `tagpu_vk.on`) — Phase G, G19a + G19b

*(This was numbered 2.20 until 2026-09-15, which §2.20 — landing 4b's fog grids — already was; §2.21c's "the static fixture of §2.20" means that one.)*

**It touches no engine address, and it must never need to.** `tagpu_vk.c` is not on
`thread-split.allow` and every value it uses arrives as an argument from `ogl_render` — which is
Phase G standing constraint 1 ([roadmap](roadmap.html)) and the line that decides whether the
renderer can ever be lifted into another process. So it has no row in the hook map above.

**Where it sits in the frame.** One call, in `ogl_render` immediately before the GL swap:

```c
if (!tagpu_vk_frame(g_ddraw.hwnd, g_ddraw.render.width, g_ddraw.render.height, g_config.vsync))
    SwapBuffers(g_ogl.hdc);
```

It returns 1 only when Vulkan presented the frame itself, and the GL swap is then skipped — two
backends must not both present in one frame. With `tagpu_vk.on` absent it returns 0 on a cached
lever read (250 ms, as elsewhere in the fork) before touching anything.

**Route D: Vulkan has its own window** — and the other two routes are not fallbacks, they are
broken on system wine. The measurement, the route table and the reason the API's "yes" could not
be trusted are in [roadmap](roadmap.html) Phase G, *Coexistence*. What the module does with it:
an owned top-level popup over the game window's client rect, `WS_EX_NOACTIVATE |
WS_EX_TOOLWINDOW` so it never takes focus or enters the taskbar, `HTTRANSPARENT` so a click falls
through to the game window, created / moved / destroyed **on the window thread** (posted
`WM_TAGPU_VK`, and `WM_WINDOWPOSCHANGED` on the owner is what keeps it in place — event-driven,
so the render thread polls no geometry). The observer in `wndproc.c` claims no message.

**The five-state machine is the whole of the thread safety**, and it is an ordering rather than a
lock:

| state | who may touch the Vulkan objects |
|---|---|
| `ST_OFF` | nobody; nothing exists |
| `ST_STARTING` | the worker that took the lane, alone (a bring-up, or the enumeration) |
| `ST_READY` | the render thread, alone |
| `ST_FAILED` | nobody; the objects are already gone |
| `ST_ZOMBIE` | **the abandoned worker, alone** — it is still the sole owner of everything it built, which is what lets it put its own objects back and hand the lane on |

(That last row said "nobody" until the second review pass, and believing it is
what put a `DestroyWindow` in `tagpu_vk_render_stop`'s tail underneath a
zombie worker's live surface.)

`OFF → STARTING` is an `InterlockedCompareExchange` taken *before* a worker is created, so two
frames cannot start two workers **and the enumeration worker takes the same grant** — that last
clause is the landing review's, and without it the two workers overlapped and shared `s_mod`,
`s_gipa` and the whole instance-level dispatch table, which belongs to an instance. `STARTING →
READY` is a **compare-exchange** the worker does after every field is written (also the review's:
an unconditional store could stamp `READY` over an `ST_ZOMBIE` set behind its back). `READY → OFF`
is the render thread's alone. So no two threads touch `s_vk` — **and, since the enumeration is
inside the same grant, no two touch the dispatch table either.**

**The bring-up is on a worker thread and that is not a preference.** `vkCreateInstance` loads the
ICD, so it *is* a `LoadLibrary`, and [field notes](field-notes.html)'s rule — load from your own
thread, never from `DllMain` or mid-present, through `real_LoadLibraryA` — was paid for once
already by the companion-DLL design.

**`ST_ZOMBIE` is a worker winding down, and NOTHING WAITS FOR IT.** An earlier shape had
`tagpu_vk_render_stop` wait five seconds for a worker still in `ST_STARTING`, then abandon and
leak its objects. The review killed both halves. The game thread waits `INFINITE` on the render
thread across a mode change (`dd.c`) and the render thread was waiting on the worker, so a mode
change that caught a bring-up in flight stalled the **lockstep world** for as long as the
bring-up had left — 371–451 ms, routinely — and because the window thread *is* the game thread
here, a winevulkan call that reached the window with an inter-thread send would have closed the
cycle game → render → worker → window with only that timeout to break it. Load-bearing, which the
note claimed it was not.

Now `render_stop` marks the lane `ST_ZOMBIE` and returns at once. `ST_ZOMBIE` means *a worker is
finishing and nobody else may touch anything* — so the worker, which is still the sole owner of
everything it built, puts its own objects back, destroys its own window and hands the lane to
`ST_OFF`. Nothing waits, nothing leaks, and the lane can be brought up again afterwards instead
of staying down for the session.

**The window belongs to the lane while it is up or coming up, and to nobody otherwise.** That is
one function, `vkw_release_unless_worker`, and it exempts exactly `ST_STARTING` and `ST_ZOMBIE`
— the two states in which a worker may hold a surface on the window and takes it down itself.
The rule was spelled out at four call sites with three different guards until the second review
pass, and two of them were wrong: `render_stop` destroyed the window under a zombie worker's
surface, and an armed lane whose bring-up **failed** kept a shown, never-painted popup over the
client area for the rest of the session.

**The residual at `WM_DESTROY` is wider than it was, and this is the statement of it.** While
`render_stop` waited for its worker, stopping the render thread implied the worker was done, so
the handler was a backstop that found nothing up. It no longer implies that: an ordinary
shutdown or mode change can run `DestroyWindow` on the game thread while an abandoned worker is
still putting a surface on the window — and Windows destroys an owned popup with its owner
whatever we do. Acquire then returns `VK_ERROR_SURFACE_LOST_KHR`, which the lane now treats as
fatal, so the outcome is a lane that comes down rather than one that spins. The orders that
would close it are a cross-thread block (the deadlock this module is arranged to avoid) or a
window outliving its owner (which Windows does not allow), so it is named rather than fixed.

**What the lane costs, measured in game** (640×480, 1024×768 and 1920×1080, shell → game →
shell): the Vulkan window tracks the client rect **exactly** (bbox identical to `xwininfo`'s, 2
073 600 of 1920×1080 px magenta), bring-up is **371–451 ms** on the worker, peak committed grows
**+5.3 to +6.5 MB**, and the **largest free VA block does not move at all** (247.4 MB before and
after) — which is the number that matters in a 32-bit process, because TA's allocator fails by
failing rather than by saying anything.

**The levers.**

| file | what |
|---|---|
| `tagpu_vk.on` | arms the lane. Optional token `color=r,g,b` moves the clear colour off the default magenta |
| `tagpu_vk.off` | turns the WHOLE module off, the GPU enumeration included — the control for an A/B against a pre-G19 DLL, and it beats `.on` |
| `tagpu_vk.gpus` | written by the enumeration worker: one line per device, `<flag> <name>`, where the flag is 1 for `DISCRETE_GPU` and 0 otherwise. The menu reads it at the NEXT attach |
| `tagpu_vk.cfg` | `gpu=<name>` — the player's choice, by NAME so adding or removing a card cannot silently re-point it |

**G19b's row is in §2.12's front-end table.** Two bounds are worth repeating because both were
found by building it. A device name is canonicalised and truncated on the way in — it comes from
the driver and lands inside a generated `.GUI` where a pipe and a semicolon are syntax. And the
list is capped at **eight** devices, which is **ours and not the engine's**: it said four, on
[GUI gadgets](gui-gadgets.html) 10.2's claim that a stage button cannot carry more stages than
`commongui.stagebuttnN` has art for, and this landing's review disproved both. `0x4A8003` clamps
the art index before the name is built, so a row past four stages draws the four-bar plate and
works — and the shipped `UI scale` row has carried six since G18f. Past the fourth device the
plate's bar count saturates while the caption stays right, which is the half that says which
card. The caption is clipped to the **120×20 plate** whatever width the gadget
carries ([GUI gadgets](gui-gadgets.html) 10.2), which is why `build_gpu_text` drops the longest
leading run of whole words every listed device shares rather than trying to widen the row.

### 2.25 The shader pipeline (`tools/spirv-gen.py`, `tools/spirv-check.sh`, `tagpu/ddraw/inc/spirv/`) — Phase G, G19c

**The GLSL does not move, and that is the design rather than an economy.** The fork's shaders live
as C string literals inside the pass that owns them, next to the comment that explains the maths.
A Vulkan lane carrying its own edition of them would be a second copy of twenty-one programs that
nothing forces to agree, and Phase G's whole method is that the GL lane is the ORACLE for the
Vulkan one — two shaders that disagree cannot be each other's oracle. So `tools/spirv-gen.py`
reads them back out, transforms them and compiles them:

```
the C string --(cc -E)--> GL 3.3 GLSL --(transform)--> Vulkan GLSL --(glslang)--> SPIR-V
                                                                   --> uint32_t[] C header
```

**Through the C PREPROCESSOR, not a regex.** `tagpu_terr.c`'s vertex shader splices
`TAGPU_EDGE_NUDGE` into the middle of a line, so the extraction has to expand macros exactly as
the compiler does. `$(CC) -E` is what does it — and its `# <line> "<file>"` markers have to be
skipped, because the quoted FILE NAME in one of them is otherwise spliced into the middle of a
shader, which compiles and then draws something wrong.

**The invariant, checked on every run rather than argued.** Strip the global-scope `in` / `out` /
`uniform` declarations out of a shader and out of its translation, and the two must be
**byte-identical** — every function, every constant, every line of every body. That residual is
**75 % of the source text** (27 687 of 36 744 bytes over the thirty-four). The transform may touch
declarations and the two built-in renames below, and the build fails if it ever touches anything
else. Verified not to be vacuous: a one-character edit inside a body is caught.

**The transform, in full.**

| | |
|---|---|
| `#version 330 core` → `#version 450` | |
| non-opaque uniforms → one **unnamed** `std140` block | unnamed, so every reference in the body still reads `uGame` and no body line changes. The block's std140 offsets are computed and written into the generated header beside the code, because the C side has to fill that buffer and the offsets are the contract |
| samplers and named blocks → a `binding` | |
| **bindings are allocated BY STAGE, not per program** | vertex: 0 = the globals block, 1.. named blocks, 8.. samplers; fragment: 32, 33.., 40.. . `QVS` is the vertex stage of six programs, so a per-program allocation would need the same source compiled six times and two programs could then disagree about what binding 3 is. The gap means a vertex and a fragment stage can never collide whatever either declares |
| varyings → explicit locations, **decided by the vertex stage** | each vertex `out` takes locations in declaration order; a fragment `in` of the same name takes the number ITS vertex stage gave it, so a fragment stage that declares a subset, or in another order, still matches. A fragment input with no matching vertex output is an error here rather than a link failure in a driver |
| fragment outputs → locations in declaration order | which is the order `glDrawBuffers` addresses them in under GL |
| `gl_VertexID` / `gl_InstanceID` → `gl_VertexIndex` / `gl_InstanceIndex` | the same value for every draw the fork issues (`firstVertex` and `firstInstance` are 0 everywhere) |

**What it deliberately does NOT do**, because each is pipeline state and a source edit would make
the shader disagree with its own GL oracle: the **Y flip** (a negative viewport height — G19d),
the **depth range** (GL maps clip z [−1, 1] to [0, 1], Vulkan takes [0, 1] directly, and every
shader here already writes a z in [0, 1], so under GL the near half of that range is thrown away
and under Vulkan it is not — a pass that depth-tests must account for it), and **`gl_FragCoord`'s
origin** (lower left under GL, upper left under Vulkan; a flipped viewport puts it back).

**The count: 34 shader sources in 21 programs, not 37.** The roadmap's 37 was a `grep -c '#version
330 core'`, and three of those hits are not shaders — a comment in `tagpu_terr.c`, a comment in
`tagpu_restore_glsl.h`, and the `_snprintf` in `tagpu_restoreglsl.c` that builds a runtime prefix.
Twenty-one programs over thirty-four sources because six of them share `QVS` and two share
`tagpu_native`'s fragment stage. **39 649 SPIR-V words** in eleven headers, 546 KB of text.

**The one cross-module pairing is a real check.** `tagpu_posedraw`'s vertex stage is linked with
`tagpu_native`'s fragment stage (`tagpu_native_unit_fs()`), so that fragment shader's varying
locations are computed from two different vertex stages and the generator fails if they disagree.
They agree.

**Where the translation runs: NOT in the build.** The roadmap's gate asked for build-time
translation and named this as the fallback; it is the better answer rather than a retreat. The
build has four entry points — `tagpu/ddraw/Makefile`, the CI job that runs it, `build.cmd` and the
MSVC project — and only the first two are ours. A GLSL compiler in the build breaks the other two
outright and puts a 30 MB toolchain between a contributor and a DLL, to translate text that
changes when a shader changes, which is to say almost never. So the SPIR-V is generated on a desk
and committed as text (`uint32_t[]` C headers, so `.publish-allow` has nothing to refuse).

**What stops committed generated code from rotting**, which is the only real objection to it.
Every header carries, per shader, **three** hashes: the SHA-256 of the Vulkan GLSL it was compiled
from, the SHA-256 of the **words themselves**, and the hash of the tool's own transform.
`tools/spirv-check.sh` — a prerequisite of `ddraw.dll` in the Makefile, in the same place and for
the same reason as `thread-split-check.sh` — re-runs the extraction and the transform (the C
preprocessor and python3, **never glslang**), compares all three, and then syntax-compiles each
header standalone because most of these arrays are not `#include`d anywhere yet. **2.0 s**, and
both halves are verified: one character changed in a shader fails the build naming that shader,
and so does one word changed inside a committed array.

**The words hash is the landing review's.** The gate covered only the GLSL, so a hand-edited,
truncated or badly-merged array passed `--check`, passed `-fsyntax-only`, and would have shipped.
What is still *not* asserted — and cannot be without the compiler — is that these words are what
glslang would emit today from that GLSL; the compiler is pinned by version and by hash instead.
**python3 is a build dependency of the Makefile** because of this check, and the CI job names it
explicitly rather than relying on the runner image having it.

**Four bounds the generator did not state and now does**, all from the review, none reachable by
today's shaders and not one of which would have announced itself:

| | |
|---|---|
| a census of the `"#version` literals in each preprocessed source against the shaders extracted from it | a shader written in a shape the extractor cannot read is an ERROR, not a silent omission — and it would have escaped the "a shader no program uses" guard as well |
| a refusal past **seven** named uniform blocks in one stage | the eighth would run into that stage's sampler bindings, and two descriptors at one binding is a validation error nobody here runs a layer to see |
| a fragment input must agree with its vertex output in **type, array size and interpolation qualifier**, not only in name | otherwise it is a pipeline-creation failure on a device rather than an error here |
| the brace depth kept honest on the rewriting path | a line carrying both a declaration and a brace would have made the rest of the shader look like global scope |

**glslang is pinned by version AND by hash** in `tools/glslang-vendor.json` (16.6.0), fetched by
`tools/glslang-fetch.sh` into gitignored `tools/glslang/` — a dev-loop tool, the same shape as
`tools/ghidra/` and `tools/vendor/`.

**The restorer's five shaders are NOT among the thirty-four**, and the reason is a fact about them
rather than a shortcut. `tagpu_restore_glsl.h`'s shaders are compiled at run time under a prefix
the DEVICE decides — `NK` from `GL_MAX_UNIFORM_BLOCK_SIZE` and `GL_MAX_DRAW_BUFFERS` divided by
the weights' widest k-block, `WMAX` from `NK` times a number read out of the weights file. `NK`
changes how many fragment outputs the conv pass declares (`#if NK > 1` … `> 4`), so it cannot be a
specialisation constant, and pre-compiling means enumerating a cross product of a device limit and
a data file. That is a decision about what the shipped weights are allowed to be — G19e's, when
the restorer pass is actually ported.

### 2.26 The FPS readout, drawn by Vulkan (`tagpu_vk_fps.c`, `tagpu_vk_pass.h`, `tagpu_vk_shot.c`) — Phase G, G19d

The first pass of the renderer to run end to end on the second backend, and the smallest one that
exercises a buffer, a texture, a shader and a draw with nothing depending on it.

**No engine address, and no new one was read to build it** — so there is nothing for
[exe reverse engineering](exe-reverse-engineering.html) in this landing, and nothing for the hook
map above either. Neither new file is on `thread-split.allow` and neither may ever need to be
(Phase G standing constraint 1); the list is **unchanged** by this landing, at the 34 entries
`thread-split-check.sh` reports. Everything either one uses arrives
as an argument: the frame's vertices from `tagpu_fps.c`, the atlas from `tagpu_text.c`, the device
and the render pass from `tagpu_vk.c`.

**MEASURED 2026-09-15**, on `scenarios/selbox-facings` under system wine on the reference setup's
4070: **0 differing pixels of 786 432 at 1024×768 and 0 of 2 073 600 at 1920×1080** — 89 and 92 ink
pixels on each side, and the two capture files are byte-identical. **0 again after the lever was
cleared and re-armed**, which exercises the whole teardown and rebuild.

**It is not a second implementation of the pass**, and that is what makes the number mean anything:

| | where it comes from |
|---|---|
| the vertices | `tagpu_fps_quads` — the quads the GL lane just drew, handed over **exactly once** |
| the texels | `tagpu_text_atlas` — the same 128 KB `tagpu_text_tex` uploads to GL |
| the shader | `inc/spirv/tagpu_fps.spv.h`, generated from the GL string by §2.25 |
| the frame size, the ink | the same numbers |

So what the comparison compares is two RASTERISERS. If this file rebuilt the quads, 0 px would
only mean two pieces of arithmetic agreed.

**Handing the vertices over ONCE, and exactly what that buys.** The overlay does not run on every
path that reaches the swap. Consuming makes it impossible for one frame's vertices to be drawn
TWICE, which is the case that matters: the second draw would be of a readout already superseded.
It does *not* make it impossible for the lane to draw the newest vertices there are on a frame
`tagpu_fps.c` was skipped on — that frame shows a readout one frame stale, which is a digit and
not a fault. **This paragraph claimed the stronger property until the landing's review disproved
it**; making it true would need a frame stamp the two files share, and nothing yet needs one.

**The Y flip is pipeline state, never a source edit.** GL's clip space has +Y up and Vulkan's has
+Y down, so the vertex shader — byte-identical to the GL one below its declarations — puts the
readout at the bottom of the frame, mirrored. The fix is a **negative viewport height**
(`VK_KHR_maintenance1`, asked for by name in `vkCreateDevice` and the pass refuses to arm without
it, saying so). Flipping the geometry instead would mirror every glyph, because the texture
coordinates travel with the vertices; flipping the shader would make it disagree with its oracle.

**The atlas upload waits for the device**, and that is a fence rather than a hope — but its cost is
real and is stated rather than implied. The atlas changes when a string is rasterised into it for
the first time, about twenty times in a session, and by then earlier frames may still be sampling
the image; a barrier in this command buffer orders nothing about submits already in flight. So the
upload calls `vkDeviceWaitIdle` first and re-sends the whole image from
`VK_IMAGE_LAYOUT_UNDEFINED`. **What that costs:** this is the render thread, the game thread waits
INFINITE on it across a mode change, and `vkDeviceWaitIdle` takes no timeout — so the stall is the
drain of two or three frames in flight on a healthy device, and as long as the wedge on a wedged
one, which is the exposure the lane's own fence wait bounds at a second and this one does not. The
by-design alternative is a second image, swapped when every slot has turned over, which moves the
same in-flight problem up to the descriptor sets for twenty events a session. **§2.28 answered
that for a pass that uploads every frame, and the answer was simpler than a swap**: one image per
FRAME SLOT, which the seam's fence already proves free, so there is no in-flight problem to move.
This file was not changed to match — twenty stalls a session buys nothing back, and the readout's
atlas is 128 KB against a viewport. **A failed wait is not an upload**: the
generation is left unclaimed and the lane comes down on the next fatal result.

**One buffer set per FRAME SLOT**, and what proves slot *i* is free is the seam's fence wait at the
top of its present — not a frame count, not "the GPU will have finished by now". `TAGPU_VK_SLOTS`
is defined once in `tagpu_vk_pass.h` and `tagpu_vk.c`'s `MAXIMG` is defined from it, so the two
cannot drift.

**`tagpu_text.c` gained its own counter, and it is not `s_dirty`.** There are two consumers of that
128 KB now, and `s_dirty` is CONSUMED by the upload that reads it — whichever consumer ran second
would never see a raster land. `tagpu_text_atlas(&gen)` returns a counter that ticks when the
PIXELS change and is never cleared; it deliberately does not tick in `tagpu_text_glreset`, because
a lost GL context does not change a byte of the atlas.

**The seam is still one file.** `tagpu_vk_pass.h` is what a ported pass is handed: an instance, a
physical device, a device, a render pass, a slot count, the flip flag and the two `GetProcAddr`s.
Nothing in it names a window, a surface or a swapchain, so `tagpu_vk_fps.c` would draw into an
offscreen image or another process's image unchanged — standing constraint 3 holding rather than
asserted. **Every pass resolves its own entry points**: the presentation table (swapchain, acquire,
present, fences) and a pass's (pipelines, descriptors, buffers, images) barely overlap, and one
shared table would have to be the union of every pass ever written.

**What changed inside `tagpu_vk.c`.** A `VkRenderPass` per DEVICE — one colour attachment in the
swapchain's format, `initialLayout` `TRANSFER_DST_OPTIMAL` (which is what `vkCmdClearColorImage`
left it in) and `finalLayout` `PRESENT_SRC_KHR`, so it performs both transitions the old code did
by hand, ordered by an external subpass dependency from `TRANSFER` to `COLOR_ATTACHMENT_OUTPUT`.
It is per device rather than per swapchain so that a pass's pipeline survives every resize; the
image views and framebuffers follow the swapchain, and a rebuild that came back with a different
format brings the lane down and says so rather than drawing into a lie. `prepare` runs OUTSIDE the
render pass and `record` inside it, because a texture upload is a transfer and a transfer may not
be recorded inside a render pass.

**A synchronisation fault found while adding it, and fixed.** The acquire semaphore was waited on
at `COLOR_ATTACHMENT_OUTPUT` alone — while the first thing the command buffer does to the image is
`vkCmdClearColorImage`, which runs at `TRANSFER` and is **not** later in the pipeline order. So the
clear was free to run before the acquire had handed the image over: a write to an image the
presentation engine may still be reading. It does not throw; it tears a frame now and then on a
driver that happens to overlap. The mask is now `TRANSFER | COLOR_ATTACHMENT_OUTPUT`.

**The oracle, and why it is not `tacli glshot`.** Route D gives Vulkan its own window, so nothing
on the GL side can see what Vulkan drew — only Vulkan can. `tagpu_fps.ab` makes both lanes capture
one frame over a black field: the GL pass clears the frame, draws, reads it back with
`glReadPixels` and writes `tagpu_fps_gl.ppm`; the lane copies the swapchain image it just presented
into a host-visible buffer and writes `tagpu_fps_vk.ppm` (`tagpu_vk_shot.c`, which allocates the
buffer when the lever fires and gives it back as soon as the file is written — 8.3 MB at 1080p, in
a 32-bit address space). `tools/vk-ab.py` diffs the two and **refuses two captures of different
sizes** rather than scaling one, because a scaled comparison cannot be 0 px by construction.
Set `color=0,0,0` in `tagpu_vk.on` so the two backgrounds match.

**What the teardown rests on, said once.** `vk_down` and `vk_resize` free everything behind a
`vkDeviceWaitIdle`, and that call CAN fail — `VK_ERROR_OUT_OF_HOST_MEMORY` above all, in the
32-bit address space this lane spends its budget measuring — returning without the device being
idle. The capture's staging buffer is the one thing whose ownership this landing newly made rest
on it, so its result is read there: a wait that did not succeed **leaks** the buffer instead of
freeing it under a copy that may still be running, which is the same trade `ST_ZOMBIE` makes (a
leak is recoverable, a free is not), it is logged, and it costs at most one capture for the
session. The rest of that teardown — the swapchain, the per-image objects, the device — has always
rested on the same wait and is unchanged here; naming it is not fixing it.

**The capture adds no wait, and the swapchain images are created so that it is legal.** Two things
the review changed, both real. The copy is recorded into frame slot *i* and completed by the fence
the seam **already** waits on at the top of the next frame that reaches slot *i* — `nimg` frames
later, a few milliseconds, no second mechanism and nothing that can free a buffer the GPU still
owns. And the swapchain is created with `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, gated on
`caps.supportedUsageFlags` so that a surface refusing it loses the capture and keeps the lane:
`vkCmdCopyImageToBuffer` requires that usage at image CREATION and no layout transition confers
it, the reference ICD allowed it anyway, and with no validation layer running the 0-px result
rested on undefined behaviour until this was fixed.

**The lever puts back every piece of GL state it touches** — the clear colour, the scissor enable,
`GL_PACK_ALIGNMENT`. It did not, and a lever that exists to measure the renderer is the last thing
that should change it.

**Both captures are of the same frame, or there is only one of them.** The first shape had each
lane poll the lever for itself — and they poll on different cadences (30 frames against 250 ms)
while the readout changes its number twice a second, so the two could have landed hundreds of
frames apart and differed in the digits while agreeing about everything else, which reads exactly
like a broken port. The flag travels WITH the vertices now, and the Vulkan pass claims it only
after every reason not to draw is past: a frame the pass cannot draw loses its half of the pair
rather than capturing a bare clear against a GL half that has text, which would have reported every
text pixel as differing — a port failure that is really an oracle failure. *(This paragraph said
"by construction" and the review's second pass disproved that too: the flag was claimed the moment
the quads arrived, with four `return 0`s still ahead of it.)*

**Constraint 4, measured rather than argued.** With `tagpu_vk.off`, this DLL's GL frame against
main's (`a043b05`) on the same fixture, same camera, pointer parked in the same place: **46
differing pixels of 786 432, all of them inside x 25..43, y 8..14** — the DIGITS of the frame-rate
readout, two processes running at different rates. Every other pixel is identical.

**What the pass allocates**, sized by the swapchain's image count (four on the reference setup):
one host-coherent vertex buffer of **18 KB** (`TAGPU_FPS_MAXV` = 288 vertices × four floats × four
slots), one **512-byte** uniform buffer (a 128-byte stride a slot, which is
`minUniformBufferOffsetAlignment` × 2), a 512×256 `R8_UNORM` device-local image (**128 KB**) and a
128 KB staging buffer — four allocations, persistently mapped where they are host-visible. Too
small to show in the lane's VA figures, which are unchanged at **+6.4 MB** peak committed
(95.9 → 102.3 MB) with the largest free block still **247.4 MB**.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes — so
the barriers, the stage masks and the layout transitions are argued from the specification and
from a correct picture, not verified by a layer. The stage-mask fault above is what that gap looks
like when it bites, and it was found by reading rather than by a tool. Also not covered: any
resolution but 1024×768 and 1920×1080, any device but the 4070, and Windows.

### 2.27 What G19c + G19d's review changed — 2026-09-15

Two independent read-only reviewers on `main...HEAD`, one on correctness and one on
synchronisation and lifetime alone. **Both found the same two HIGH defects independently**, which
is the strongest thing that can be said for running two.

| | what it was | why it mattered |
|---|---|---|
| **HIGH** | the swapchain images were never created with `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, and the capture copied from them anyway | `vkCmdCopyImageToBuffer` requires that usage at image CREATION; no layout transition confers it. The reference ICD allowed it and no validation layer was running, so **G19d's entire 0-px result rested on undefined behaviour** — on another driver the oracle reads garbage or faults. It is asked for against `caps.supportedUsageFlags` now, and a surface that refuses it loses the capture rather than the lane |
| **HIGH** | the capture waited up to a second on the frame's fence and, **on the timeout**, destroyed the staging buffer the already-submitted copy writes into | the GPU would then have completed a copy into freed memory, so the one-second timeout was load-bearing rather than the belt the comment claimed. It is also exactly the shape [CLAUDE.md](https://github.com/Code-Herder/ta-impure-patch/blob/main/CLAUDE.md) forbids — a correctness argument made of timing. There is no wait at all now: the copy is completed by the fence the seam **already** waits on at the top of the next frame that reaches that slot |
| MEDIUM | the A/B flag latched when the Vulkan lane never consumed it | set once, cleared only on consumption — so on a frame that did not reach `tagpu_vk_fps_prepare` (the lane not yet `ST_READY`, a swapchain rebuild, the lever off) the flag rode a LATER frame's vertices and the two captures were of different frames. Which is the one thing the design exists to prevent, and §2.26 said so. Its life is one frame now |
| MEDIUM | the freshness gate hashed the GLSL and not the SPIR-V | a hand-edited, truncated or badly-merged word array passed `--check` and `-fsyntax-only` and would have shipped. A `words` hash per shader closes it; verified by corrupting one word |
| MEDIUM | `VK_INCOMPLETE` was treated as a failure when enumerating device extensions | a driver with more than 512 extensions returns it with a perfectly good partial list; the lane would have reported that `VK_KHR_maintenance1` "is not offered" on a device that offers it and refused **every ported pass for the session**. The list is counted and allocated now, and `VK_INCOMPLETE` is an answer — which also takes 130 KB off a worker's stack |
| MEDIUM | `vkDeviceWaitIdle` in the atlas upload was described as costing nothing | the ordering is sound; the cost is a render-thread stall with no timeout, on the thread the game thread waits INFINITE on. Stated now, with the numbers and with the alternative that was not taken and why |
| LOW | the capture's barrier was sourced at `COLOR_ATTACHMENT_OUTPUT` | the render pass performs its `finalLayout` transition at `BOTTOM_OF_PIPE`, which is later, so the barrier was not ordered after it |
| LOW | the A/B lever left the clear colour, the scissor enable and `GL_PACK_ALIGNMENT` changed for the session | small in practice, and precisely the kind of leak from a measuring lever into play state that an A/B six months from now would be reading |
| LOW | `spirv-check.sh` reported "could not run" (exit 2) as "stale" (exit 1) | it would send someone to regenerate headers that are perfectly current |
| LOW | the extractor skipped a shader whose literal began on the declaration's own line | no shader does that today, and a tool that silently SKIPS one is worse than a tool that refuses it — it would escape the "a shader no program uses" guard too. A census makes it an error |
| LOW | `s_abShot` survived a teardown | benign, and gone with the wait it belonged to |

**Three claims in the notes were findings in their own right** and are corrected in place: that the
vertex hand-over made staleness impossible "by construction" (it makes a *repeat* impossible; a
frame the overlay skipped draws one-frame-stale digits), that the staging buffer was "owned by the
GPU whatever the present then said" (it was, which is why freeing it on the timeout was the bug),
and that the freshness gate covered the generated headers (it covered their input).

**What survived verification without a finding**, because it is worth knowing which parts were
actually checked: the std140 offsets — one reviewer decoded `OpMemberDecorate … Offset` out of all
thirty-four committed arrays and compared them to the comments, **0 mismatches**; the descriptor
layout against the SPIR-V's own decorations; that `fi = frame % nimg` with the fence wait on
`fence[fi]` really does prove that slot's buffers free (a bound, not timing); that
`tagpu_vk_fps_down` gives back exactly what was built including the mapped-but-failed path; that
no handle or entry point from one `VkDevice` can reach another; `tagpu_text.c`'s generation
counter; `tools/vk-ab.py`'s parser and the agreement of its verdict with its exit status; and that
the diff contains no engine address and no byte patch.

**A SECOND PASS on the rework** — because two of the fixes changed the synchronisation design
rather than patching it, which is the class that ships silently. It confirmed both HIGH fixes hold
and found five more, one of them the rework's own:

| | |
|---|---|
| the rework's own | `vkDeviceWaitIdle`'s result was discarded in `vk_down` and `vk_resize`, and that result had just become the WHOLE safety argument for freeing the capture's staging buffer — the same defect one call further along. It is read now, and a wait that did not succeed leaks the buffer rather than freeing it under a live copy |
| widened by the rework | the A/B flag was claimed the moment the quads arrived, with four `return 0`s still ahead of it (one of them added by the rework), so a frame the pass could not draw would have been captured as a bare clear against a GL half with text — every text pixel reported as differing, a port failure that is really an oracle failure |
| | the state save and restore added three entry points to the A/B's list, and a context missing one made the lever do nothing at all, silently, retrying every poll for the session |
| | a failed row-buffer allocation left a header-only PPM on disk, which reads as a broken writer rather than as a machine out of memory |
| | the words hash was keyed off the comment above each array rather than the array's own symbol, so a header whose two had drifted apart could hash one shader's words under another's name |

Its verdict on the two HIGH fixes, which is the reason the pass was run: `TRANSFER_SRC` **holds**
(`cansrc` is re-derived on every swapchain creation, `vk_down`'s memset fails it closed, the single
use site is the caller so a refusing surface loses the capture and not the lane); the capture
lifetime **holds** on every enumerated path — lever cleared, any fatal result, `OUT_OF_DATE`,
`SUBOPTIMAL`, mode change, window destroyed, GPU row changed, `ST_ZOMBIE`, render-thread stop —
"nothing leaks; nothing frees early except through the unchecked waitIdle", which is the finding
above and is now closed.

**Still not covered, and unchanged by either pass:** no Vulkan validation layer ran. Every finding
above was found by reading the specification against the code. The first HIGH is what that gap
costs when nobody is reading.

### 2.28 The scaffold overlay, drawn by Vulkan (`tagpu_vk_scaffold.c`, `tagpu_abshot.c`) — Phase G, G19e

The first of the **world passes**, and the one chosen first because it is the smallest of them
(a fragment stage of 430 words, one sampler, one 16-byte uniform block) *and* because it forces
the question G19d was allowed to leave open: it uploads a **viewport-sized image every frame**,
where the readout's atlas changes about twenty times in a session.

**No engine address, and no new one was read to build it** — so there is nothing for
[exe reverse engineering](exe-reverse-engineering.html) in this landing and nothing for the hook
map above. Neither new file is on `thread-split.allow` and neither may ever need to be (Phase G
standing constraint 1); the list is **unchanged** at the 34 entries `thread-split-check.sh`
reports. Everything both files use arrives as an argument.

**MEASURED 2026-09-15**, on `scenarios/feat-forest` (151 tall features in the sweep) under system
wine on the reference setup's 4070: **0 differing pixels of 786 432 at 1024×768 and 0 of
2 073 600 at 1920×1080**, with **190 247 and 377 020 non-black pixels on *each* side** — a
quarter of the frame is scaffold ink, so this is not two blank frames agreeing, which is the
failure mode `tools/vk-ab.py` exists to name. The capture files are byte-identical. **0 again
after the lever was cleared and re-armed**, and **0 again after a full free-and-rebuild of every
per-slot resource inside one lane** (below).

**It is not a second implementation of the pass:**

| | where it comes from |
|---|---|
| the scaffold bytes | `tagpu_scaffold_overlay` — the buffer the GL lane just uploaded to its own texture, handed over **exactly once** |
| the quad | `TAGPU_SCAF_QUAD` in `inc/tagpu_scaffold.h`, **one literal both lanes build their vertex buffer from** |
| the NDC rect, the row count | the numbers the GL draw passed to `uRect` and `uRows` |
| the shader | `inc/spirv/tagpu_scaffold.spv.h`, generated from the GL string by §2.25 |

#### The per-frame upload, which is this landing's real work

§2.26 named the by-design alternative to `vkDeviceWaitIdle` and left it to this gate. It turned
out to need **no new mechanism at all**:

> **One image and one staging buffer per FRAME SLOT**, and the seam's fence is what makes writing
> them safe.

The invariant is the one `tagpu_vk_pass.h` already publishes for buffers, used for an image:
when `prepare` is called with `slot`, the seam has just waited on `fence[slot]`, so the submit
that last used that slot's resources has **completed**. Nothing of ours is in flight for it.
Three things follow for the same one-line reason, none of them needing a device-wide wait:

* the CPU may write that slot's staging buffer — a barrier could not help here anyway, because
  it orders GPU work and the hazard is a CPU write;
* the copy into that slot's image has no prior access to order against, so `oldLayout` is
  `UNDEFINED` (the whole image is re-sent, so there are no contents to preserve) and
  `TOP_OF_PIPE` with an empty source access mask is correct rather than lazy;
* the descriptor set naming that image may be rewritten, which is what makes a **viewport change
  free**: the slot is rebuilt at the moment we are handed it, which is the moment we own it. No
  retire list, no deferred free, no second wait. (The game's viewport changes without the
  swapchain changing — a shell/game transition alone does it.)

**What it costs, and the viewport is not the screen.** Measuring that rather than assuming it
halves the figure: a 1024×768 screen has an **896×704** viewport, so 630 784 bytes a slot, 2.4 MB
of device-local and as much again host-visible, **4.8 MB** in all on the reference setup's
four-image swapchain; 1920×1080 has **1792×1016**, so 1 820 672 a slot and **13.9 MB**. The
image's own allocation is whatever `vkGetImageMemoryRequirements` asks for an optimal-tiled R8 of
that extent, so at or above those numbers.

**And none of it is kept once there is nothing to draw.** An early `return 0` when the GL twin
publishes nothing held every byte of that for the life of the lane after one look at the
scaffold — *the first version of this file claimed the opposite in its own header, and the claim
was false*. Slot `slot` is given back at that point for the same reason and at the same instant
the rest of the function writes it in, so after one turn of the slots the pass holds only its
pipeline, its sampler, its descriptor sets and a 512-byte uniform buffer, none of which scales
with anything. The lever cleared, the shell, a level teardown and a frame the twin skipped all
arrive there. The cost of being wrong is one `vkCreateImage` a slot when the pass comes back, and
that happens at a shell/game transition, not in a frame.

**The cheaper alternative, and why it is not here.** One image shared by every slot is *also*
correct: a barrier at the top of each upload, `FRAGMENT_SHADER`/`SHADER_READ` →
`TRANSFER`/`TRANSFER_WRITE`, orders the copy after the previous frame's sampling, because
submission order spans submits to one queue and a write-after-read hazard needs only an execution
dependency. It saves `(nimg−1)` images — 5.2 MB at 1080p — and costs three things: the image's
layout has to be tracked across frames, a viewport change has to retire the old image until every
slot has turned over, and the safety argument goes from one line to a paragraph about cross-submit
ordering. The staging buffers stay per-slot either way. **That is the design to reach for if a
later per-frame pass needs the memory back**; for the first of them, the one-line invariant is
worth 5.2 MB behind a lever.

#### The oracle is shared now, because a world pass has a background

`tagpu_abshot.c` is the GL half of every Phase G A/B, and the three world passes still owed one
get it in three lines. G19d could black the whole GL frame because the readout is the **last**
thing drawn; a world pass has the rest of the frame under it on the GL side and a bare clear
under it on the Vulkan side. So the GL twin blacks the frame **immediately before its own draw**
and reads it back **immediately after**, before the native pass and the UI layer have run. What
is compared is one pass over black against one pass over black. The player sees one frame with
everything drawn before that point missing from it; that is what a measuring lever costs, and it
is why it is one frame.

`tagpu_fps.c` is refactored onto it and **loses 111 lines** (501 → 390) — its own A/B still measures **0 px of
786 432** on the refactored oracle, which is the regression that says the move was clean.
*(This said "117 lines (501 → 384)" until the G19e re-review counted it: 384 was the file before
`fecd146` added the six-line depth-state block, and the figure was never re-taken. The ink count
that used to be quoted here is gone on purpose — those pixels are the readout's own digits, so the
number follows the frame rate on screen and is not a property of the port; see §2.30.)* One thing improves rather than moves: the review of G19d found that a context
missing any of the A/B's entry points made the lever do nothing **silently**, retried every poll
for the session. There is now one resolve, one message naming the entry point that was missing,
and one latch. (The five it needs — `glGetFloatv`, `glIsEnabled`, `glDisable`, `glClearColor`,
`glReadPixels` — are not among the fork's own globals in `opengl_utils.h`; this DLL is
`ddraw.dll` and does not link opengl32, it loads it, so even GL 1.1 is a pointer to resolve.)

#### Two ported passes made a new way to get a plausible wrong answer

Each GL capture holds **one** pass, because its twin blacks the frame around its own draw. The
Vulkan capture is **one frame**, and a frame holds every armed pass at once. So with two A/B
levers armed the diff would report the other pass's pixels as differing: a port failure that is
really an oracle failure, which is the worst answer an oracle can give.

The seam refuses to capture when **more than one pass claimed the frame, or more than one drew
into it**, and says so in the log. Both conditions are checked, and the second is the one that
does the work: verified 2026-09-15 by arming `fps.ab` and `scaffold.ab` together, the two levers
landed on **different** frames (their poll cadences differ) so each claimed alone — and the frame
was contaminated anyway, because both passes were drawing. A claim-count check alone would have
written the pair and been believed.

#### Pipeline state: blending is new, depth is not needed, and one of them is G19e's next question

**Blending** is set on **both factor pairs** (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` for colour *and*
alpha), because `glBlendFunc` sets the alpha factors as well as the colour ones. The shader
writes a 0.55 alpha and `discard`s where the scaffold is free, so over the A/B's black field the
result is 0.55 × the ramp colour on each lane — and the 0-px result is what says the two blend
units agree to the least significant bit. The Y flip is a negative viewport height exactly as in
§2.26, and there is no culling, because that flip reverses every triangle's winding. The topology
is a **strip**, which is what the GL twin draws: four vertices as a strip and six as a list cover
the same quad but not necessarily with the same two triangles, and a diagonal running the other
way puts every pixel on it on the other side of a rounding decision.

**Depth is not in this landing** — the GL twin calls neither `glEnable(GL_DEPTH_TEST)` nor
`glDepthMask`, and the seam's render pass has one colour attachment and no depth one. The passes
that **do** test are G19e's next landings, and the question is stated here rather than guessed at
when one of them starts:

> GL maps clip z from [−1, 1] onto the depth range and Vulkan takes [0, 1] directly. Every one of
> these shaders already writes a z in [0, 1], so under GL the near half of the range is thrown
> away and under Vulkan it is not. **The ordering is identical either way** — both mappings are
> affine and increasing, so every depth *test* comes out the same — but the *values* are not, and
> Vulkan gets a bit more precision out of a fixed-point buffer than GL does, which is enough to
> settle a z-fight the other way and put a 0-px comparison out of reach.
>
> The fix is **pipeline state, never a shader edit** (an edited shader would disagree with the
> twin that is its oracle), and it needs **no extension**: a viewport with `minDepth = 0.5` and
> `maxDepth = 1.0` maps clip z ∈ [0, 1] onto exactly GL's `(z+1)/2`, values and precision
> included. `VK_EXT_depth_clip_control` with `negativeOneToOne` would do the same by adopting GL's
> convention outright, and is the answer if a shader is ever found that writes a z below 0 — Vulkan
> clips those and GL does not. **Neither is written yet**, and the render pass will need a depth
> attachment before either matters.

**Both halves of that were BUILT by the next landing and the answer held** — §2.29. The render
pass carries a depth attachment (`tagpu_vk.c`, one image per swapchain image, `LOAD_OP_CLEAR` at
1.0), and the viewport carries `minDepth 0.5 / maxDepth 1.0`. The feature pass is 0 px against its
GL twin with them and the scaffold's own A/B still reads 0 px, so the attachment cost the passes
that do not test nothing. `VK_EXT_depth_clip_control` is still unused and still the answer only if
a shader is found writing a z below 0.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes — so
the barriers, the stage masks and the layout transitions are argued from the specification and
from a correct picture. Also not covered: any resolution but 1024×768 and 1920×1080, any device
but the 4070, and Windows.

#### Constraint 4, measured — and a fixture correction worth more than the measurement

**0 differing pixels of 630 784** in the world viewport, this landing's DLL against `afceba5`'s,
both with `tagpu_vk.off`, on `selbox-facings` at 1024×768, sim paused (`tab`), pointer parked in
the side panel so its animating sprite is outside the crop.

**`selbox-facings` does NOT have a 0-px cross-launch floor at this camera**, and the note that
says it does is about a *within-run* floor. Two launches of the **same** DLL differ by **71 px at
x 1017..1023, y 236..277** — a two-state artefact in a 7×42 sliver at the frame's right edge,
which reproduced DLL-against-itself and is therefore nobody's change. It was caught only because
the first base-versus-landing pair happened to land in the same state and read 0; a second sample
of the same binary read 71. **Take at least two samples of one binary before believing a
cross-launch floor**, and mask that sliver on this fixture.

`feat-forest` is worse and cannot answer the question at all: its walking commander gives it a
**~4000 px** cross-launch floor (3985 px, same DLL, two launches), which is *larger* than the
3906 px a base-versus-landing pair reads there. It is an excellent A/B fixture for the two lanes,
because both captures are of the same frame, and a useless one for anything across launches.

Also worth knowing for any cross-launch diff on a scenario: the engine's own "**forces have been
obliterated**" messages from `clear_existing` sit in the top-left of the viewport for tens of
seconds and land differently per run — 8179 px until they expire. 35 seconds after the load is
enough — **but only if the game is RUNNING**: §2.29 found they expire on TICKS, so a `tab` sent
straight after the load freezes them on screen for as long as the pause lasts.

### 2.29 The feature pass, drawn by Vulkan (`tagpu_vk_feat.c`, `tagpu_gaf.c`'s CPU mirror) — Phase G, G19e

The **second world pass**, and the first one that **depth-tests** — which is why it went next: it
forces §2.28's depth decision, which was settled on paper and left for whichever pass needed it,
into pipeline state that a 0-px comparison can grade.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_feat_handover`, so there is nothing for [exe reverse engineering](exe-reverse-engineering.html)
in this landing and nothing for the hook map above. `tagpu_vk_feat.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15**, on `scenarios/feat-forest` (151 tall features, 180 body and 150 shadow
drawables in the sweep) under system wine on the reference setup's 4070, `ss=1`:

| | |
|---|---|
| feat A/B, 1024×768 | **0 differing px of 786 432**, **243 538 non-black on *each* side** |
| feat A/B, 1920×1080 | **0 of 2 073 600**, 506 936 non-black each side |
| both | capture files byte-identical; the 1024×768 pair reproduced **the same md5** on an independent relaunch, and read 0 again after `tagpu_vk.on` *and* `tagpu_feat.on` were cleared and re-armed — which frees and rebuilds the pipelines, the descriptor sets, the shared atlas image and every per-slot resource |
| scaffold A/B (§2.28's, the regression for the depth attachment) | **0 of 786 432**, **190 247 non-black a side — the same number §2.28 recorded** |
| fps A/B (§2.26's) | **0 of 786 432**, 89 ink px a side |
| constraint 4 | **0 of 630 784** in the world viewport against `6ad52e4` with `tagpu_vk.off` |

Nearly a third of the frame is feature ink, so this is not two blank frames agreeing — the failure
mode `tools/vk-ab.py` exists to name.

**It is not a second implementation of the pass:**

| | where it comes from |
|---|---|
| the vertices | `s_verts[B_SHADOW]` and `s_verts[B_BODY]` — the two arrays the GL gather filled and the GL upload took, handed over **exactly once** |
| the vertex layout | `TAGPU_FEAT_ATTRS` in `tagpu_feat.h`, **one literal both lanes build their vertex input from** (the GL VAO loops over it too) |
| the uniforms | the numbers the GL draw passed to `uGame`, `uZoom`, `uZoomC`, `uDepthScale`, `uRestored`, `uLit`, `uFog`, `uFogOrg`, `uFogDim` |
| the atlas texels | `tagpu_gaf.c`'s **CPU mirror** — written by the same `atlas_paint` that writes the GL texture, from the same `s_pad` rows, in the same call |
| the palette | `tagpu_pal_live()`, the buffer `s_palTex` is uploaded from |
| the fog grid | a **copy of** the packet's grid, taken at publish time into a buffer the publishing module owns — the same bytes the GL lane hands `glTexImage2D(GL_RG8, …)`, and the pointer is not the packet's. It was the packet's until the G19e landing review (2026-09-15) pointed out that `tagpu_packet_frame_end()` ends that pointer's declared lifetime *before* `render_ogl.c` runs the Vulkan lane |
| the fog shade LUT | `tagpu_native_foglut()` — the 256 bytes last uploaded to `s_fogLutTex`, identity fallback included, published rather than rebuilt |
| the shader | `inc/spirv/tagpu_feat.spv.h`, generated from the GL string by §2.25 |

#### The depth answer, built

§2.28 wrote the rule down and this landing is where it became code. Two pieces:

* **The seam grew a depth attachment** (`tagpu_vk.c`): one image **per swapchain image** — `nimg`
  frames are in flight and each has a framebuffer of its own, so one shared buffer would be
  written by two overlapping frames — `LOAD_OP_CLEAR` at **1.0** (what `glClear(GL_DEPTH_BUFFER_BIT)`
  uses on the GL side, in the same place: the top of the world FBO), `STORE_OP_DONT_CARE`,
  `UNDEFINED` in. Both subpass dependencies grew their depth halves
  (`LATE_FRAGMENT_TESTS`/`DEPTH_STENCIL_ATTACHMENT_WRITE` →
  `EARLY_FRAGMENT_TESTS`/read+write).
* **The format is 24-bit fixed point or there is none.** The GL lane's world FBO is
  `GL_DEPTH24_STENCIL8`, so a `D32_SFLOAT` attachment would settle a z-fight the other way in
  exactly the cases that are too close to call — which are the cases a 0-px comparison is made of.
  `vk_depth_format()` asks for `D24_UNORM_S8_UINT` and then `X8_D24_UNORM_PACK32`, and a device
  offering neither gets **no depth attachment at all**: the passes that do not test go on working
  and the ones that do refuse to arm and say so. On the reference setup it binds
  `D24_UNORM_S8_UINT` (the log prints `depth format 129`).
* **The range is the viewport's**: `minDepth = 0.5`, `maxDepth = 1.0`, which maps clip z ∈ [0, 1]
  onto exactly GL's `(z+1)/2`. Never a shader edit — an edited shader would disagree with the twin
  that is its oracle.

**Every pipeline in the lane now declares a depth state**, because `pDepthStencilState` may not be
null in a subpass that has a depth attachment. `tagpu_vk_fps.c` and `tagpu_vk_scaffold.c` declare
one with test and write **off**, which is what their GL twins do — and their own A/Bs re-measuring
0 px is what says the attachment cost them nothing.

**Two depth-write modes, so two pipelines.** The GL twin draws shadows under `glDepthMask(GL_FALSE)`
— they are ground decals and must occlude nothing — and bodies with it true. Two pipelines off one
layout needs no extension; `VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE` is Vulkan 1.3 or an extension and
would buy one object.

#### The scissor is the one number that is not the same on both sides

The native pass clips this draw to the world viewport, and a scissor is expressed in **framebuffer**
coordinates, which the two APIs disagree about. In the GL world FBO, framebuffer row 0 is clip-space
y = −1; with the negative viewport height this pass used until 2026-09-17, row 0 of the Vulkan image
was clip-space y = **+1**. The two images were stored the same way round — which is exactly what let
the bytes be compared at all — and a *rectangle* in one was the vertical mirror of the same
rectangle in the other:

    scissor.offset.y = H − (vpT + vh)

§2.28's "the scissor is NOT flipped" is true of a **full-frame** scissor and of a pass drawn into
the default framebuffer; this is neither, and the two statements do not conflict. The rect is also
scaled by the attachment's extent over the game frame's, which is the same division the vertex
shader does by `uGame` and reduces to the identity at the sizes an A/B runs at.

**And it travels with its ENABLE, not just its rect.** `tagpu_native_scissor_on()` reports whether
the clip is actually on (the native pass enables it only when it resolved `glScissor`). A Vulkan
lane that clipped while GL did not would differ in every feature the gather's margin reaches past
the viewport, which on a forest map is a wide band down both edges.

**[CORRECTED BY LANDING 5b, 2026-09-17 — the paragraph below was true of the comparison and hid a
defect in the lane.]** It read: *"Both halves of this A/B are upside-down pictures of the world, and
that is correct: the GL twin draws into the world FBO, whose clip-space +1 is the bottom of the
screen (the composite quad turns it over). They are upside down identically, which is the only thing
the comparison asks."* Every clause is true. What none of it covers is that the Vulkan lane has **no
composite quad** — the ported passes draw straight into the swapchain image — so making the Vulkan
picture match the GL *FBO* is what made Route D present the world upside down. The scissor above is
therefore no longer mirrored, this pass takes a **positive** viewport height, and the GL half of the
capture is written top-down (`TAGPU_ABSHOT_TOPDOWN`). §2.40 has the whole of it.

#### The atlas mirror — the mechanism the remaining world passes need

Every world pass samples an atlas, and no atlas in this tree keeps a CPU copy of its texels: the
decoder uploads and frees. So the first of them had to answer how a second backend gets those bytes.

> **`TAGPU_GAFATLAS.mirror`** — `dim × dim` bytes written by the same `atlas_paint` that writes the
> GL texture, from the same `s_pad` rows, in the same call. Not a second decode of the art: the very
> rows the `glTexSubImage2D` above it hands GL.

**It is correct from the instant it exists**, which is the part worth the paragraph. A mirror
allocated after the atlas has painted frames would hold zeros where those frames are, and a backend
uploading it would draw black trees for the rest of the session — silently, because nothing in the
atlas is wrong. So `tagpu_gaf_atlas_mirror` marks every painted entry **reserved** (`ok` 0,
`resv` 1), the state a repack leaves an entry in: the rect stays where it is and the next
`tagpu_gaf_atlas_get` paints it again, into GL and the mirror together. And because `atlas_get`
paints a reserved entry *before it returns it*, **every UV a vertex carries addresses texels that
are in the mirror** — the request is made on the GL twin's arm poll, which runs before the gather,
so the first frame that has a mirror is already correct. Entries nothing draws may stay stale in it;
nothing samples them, in either lane. Measured on `feat-forest`: `atlas mirror armed, 4096 KB — 13
painted frame(s) re-decode on their next use`.

It is asked for **on the 30-frame arm poll and not per frame**, because `tagpu_vk_armed()` is two
file-attribute queries and a pass that asked every frame would make them on every frame of ordinary
play, where the answer is no and stays no. 4 MB, so it is paid for only while the Vulkan lane is
armed, and once armed it stays for the process's life (there is no atlas destructor here).

#### The uploads: §2.28's cheaper design, taken up

§2.28 established one image and one staging buffer **per frame slot** on a one-line invariant, and
named the cheaper alternative — one image shared, behind a write-after-read barrier — saying to
reach for it when a pass needed the memory back. **This is that pass**, and the split is by size:

| | |
|---|---|
| **the atlas** | 2048² R8 = **4 MiB**. Per-slot it would be 16 MiB of device-local on a four-image swapchain, in a 32-bit address space whose largest free block is the number this phase spends its budget measuring. So it is **one image**, and the barrier at the top of an upload — `FRAGMENT_SHADER`/`SHADER_READ` → `TRANSFER`/`TRANSFER_WRITE` — orders the copy after every earlier frame's sampling. Legal across submits because a barrier's first synchronisation scope includes everything submitted to the queue before it, and a write-after-read hazard needs only an execution dependency; the access masks are for the layout transition, which is a write. Uploaded only when the mirror's serial says the bytes moved, and only the rows the shelf packer has used (`shelfY + shelfH`, published as `atlasRows`) |
| **the palette, the fog LUT, the fog grid** | 1 KiB, 256 B and 1 440 B at the measured fixture's 30×24 grid. **Per-slot**, because at that size §2.28's one-line invariant is worth more than the memory — and a dimension change (the grid's, whenever the view walks far enough) is then free: the slot is rebuilt at the moment we are handed it, which is the moment we own it. One staging buffer carries all three, and it is rebuilt **with** the fog image so the buffer and the image it feeds cannot disagree about the size |
| **the vertices** | per-slot, doubling from 64 KiB, never shrunk while the pass is drawing |

**The atlas's staging buffer is per-slot either way and cannot be anything else**: a barrier orders
GPU work and the hazard there is a CPU write. It is allocated on the frame that uploads and **given
back at that slot's next `prepare`** on which no upload is due — the same fence, one turn of the
slots later — so a settled scene holds none of it and a map still filling its atlas holds at most
one per slot while it does. *(The file header claimed this before it was true; §2.28's own lesson
— "a header claim is a claim" — is why it now is.)*

**And nothing is kept once there is nothing to draw**, exactly as §2.28's scaffold: a frame the GL
twin handed nothing over gives that slot back.

#### The A/B's oracle needed two more things, and both are in `tagpu_abshot.c`

* **`TAGPU_ABSHOT_DEPTH` — the depth buffer is cleared too.** For a pass that depth-tests the colour
  clear alone is half an oracle: the GL twin would test against what the passes *before* it left in
  the depth buffer (the terrain) while the Vulkan lane's render pass starts from a cleared one. The
  depth **write mask** is forced on for the clear and put straight back, because
  `glClear(GL_DEPTH_BUFFER_BIT)` is masked by it.
* **`TAGPU_ABSHOT_SCISSOR` — the scissor goes back before the pass draws.** The clear stays
  unscissored (a scissor left on by an earlier pass would black a rectangle instead of the frame),
  but a world pass clipped to the viewport *is* the pass and an unclipped one is something else.

Both are opt-in, so `tagpu_fps.c` and `tagpu_scaffold.c` measure exactly what they measured before.

**`ss` must be 1 for this A/B and the pass says so rather than writing a mismatched pair.** The GL
capture is the world FBO's viewport, `gw*ss × gh*ss`, and the Vulkan one is the window's client
rect; at `ss` 2 they differ by a factor of two and `tools/vk-ab.py` would refuse the pair after the
fact. `tacli arm <i> ss.off` is the lever.

#### What it does not do

**Classic++'s restored atlas is not mirrored**, so when the GL twin reports `uRestored` 1 this pass
draws **nothing** and says so once. Drawing with `uRestored` 0 instead would be a different picture
from the twin's and the A/B would report it as a rasteriser difference, which is the one answer an
oracle must never give. The twin is `tagpu_restoreglsl.c`'s RGBA8 surface; a mirror of it is the
same mechanism as the R8 one and is a later landing's. A `tacli` instance opts out of the play
defaults, so an A/B run does not meet this; a `--defaults` instance does.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes — so
the barriers, the stage masks and the layout transitions are argued from the specification and from
a correct picture. Also not covered: any resolution but 1024×768 and 1920×1080, any device but the
4070, Windows, `ss` 2, Classic++, and an **in-process map change** (every measurement here filled
the atlas from empty after a relaunch, so `tagpu_gaf_atlas_forget` and a repack have not been
watched feeding the mirror).

#### Constraint 4, measured — and the fixture trap is not the one on record

**0 differing pixels of 630 784** in the world viewport (`vp=(128,32,896,704)`), this landing's DLL
against `6ad52e4`'s, both with `tagpu_vk.off`, on `selbox-facings` at 1024×768, the full arm set,
pointer parked in the side panel, sim paused. The **cross-launch floor was measured first and read
0**: two launches of the landing's own binary differ by 0 px, and a third pairing (the base's launch
against a second launch of the landing's) also reads 0. §2.28's two-state 71-px sliver at
x 1017..1023 did not appear in any of the five launches taken here — it is a real artefact, not a
constant one, which is the whole reason the floor is re-measured every time rather than quoted.

**The trap that did bite is a different one, and §2.28 has it half right.** The engine's "forces
have been obliterated" chat lines expire on **TICKS, not on wall-clock seconds**, so "wait 35 s
after the load" is only true of a game that is *running*. Pausing with `tab` immediately after
`scenario load` freezes them on screen indefinitely: the first pair taken here read **8 282
differing px in a bbox of x 138..430, y 52..106** — the chat block — purely because one instance had
been paused at tick 155 and the other at tick 433. Let the game run until the lines go (about 60 s
at speed 10), *then* park the pointer and pause.

### 2.30 The terrain pass, drawn by Vulkan (`tagpu_vk_terr.c`, `tagpu_terr.c`'s two mirrors) — Phase G, G19e

The **third world pass**, the first **instanced** one, and the one the gate names an exact
oracle for. It is also the first whose shared textures **change size while the lane is up**,
which is the lifetime question §2.28 named and §2.29 did not have to answer.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_terr_handover`, so there is nothing for [exe reverse engineering](exe-reverse-engineering.html)
in this landing and nothing for the hook map above. `tagpu_vk_terr.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1` — and then
**RE-MEASURED IN FULL, TWICE**, on 2026-09-15: once on the binary the landing review's rework
produced, and again on the binary the **re**-review's fixes produced (md5 `7f50fa3e…`), which is
the one these figures are from and the one that would land. Both reworks touch the frame loop and
one of them touches the A/B harness itself, so a table taken on an earlier binary is a table about
a different program — that is the gate this landing failed the first time. **That md5 names a build, not a tree:**
the link is not byte-reproducible — two builds of the *identical* tree differ in exactly three bytes
(measured 2026-09-15: the COFF `TimeDateStamp` at `0x88`, the optional header's `CheckSum` at
`0xD8`, and the debug directory's copy of the stamp at `0x129804`), so re-hashing a rebuild can
never tell you whether you are on the measured binary. What can is that the *tree* has not moved —
`git status` clean at the commit the table cites — and that is the check to run before trusting a
figure here. **Every figure below
reproduced on every binary**, ink counts included; the only one that moves is the readout's, and it
moves by construction (see its row):

| | |
|---|---|
| terr A/B, 1024×768, `feat-forest` (Two Continents) | **0 differing px of 786 432**, **630 719 non-black on *each* side** |
| terr A/B, 1920×1080, same | **0 of 2 073 600**, 1 820 568 non-black each side |
| terr A/B, 1024×768, **Anteer Strait**, reached by an **in-process level cycle** | **0 of 786 432**, **630 784 non-black each side — the entire viewport** |
| all three | capture files byte-identical; a pair read 0 again after `tagpu_vk.on` *and* `tagpu_terr.on` were cleared and re-armed, which frees and rebuilds the pipeline, the descriptor sets, both shared images and every per-slot resource. That cycle has now been run on **both maps across the three binaries** — Anteer Strait (630 784 ink a side) on the rework, Two Continents (630 719) on the final one — and reads 0 every time. **It exercises `_down`'s non-owed branch and only that one**: with no refusal outstanding the pass must come back `ST_UNBUILT`, and it does. It says nothing about the owed branch, where the re-review found a real defect this run could not have caught (below) |
| feat A/B (§2.29's, the regression) | **0 of 786 432**, **243 538 non-black a side — §2.29's own number** |
| scaffold A/B (§2.28's) | **0 of 786 432**, **190 247 a side — §2.28's own number** (`tall=151` on the fixture) |
| fps A/B (§2.26's) | **0 of 786 432**. The ink count has read **88, 89, 92 and 94** across passing runs — **those pixels are the readout's own digits**, so the number follows the frame rate on screen and is not a property of the port. What the A/B asserts is that both halves carry the *same* digits, which is the 0. Do not treat this one as a regression figure; for the world passes the ink count *is* one, because there it is the scene |
| constraint 4 | **0 of 630 784** in the world viewport against **`afceba5` — `main`, i.e. the whole seven-commit landing rather than one pass's predecessor** — with `tagpu_vk.off` and the full pass set on both, over a **cross-launch floor of 0** measured by running the landing's own binary twice, and a within-run floor of 0 on all three instances |
| the owed teardown | **never fired**: 0 occurrences of `the seam tears it down` / `drained, tearing it down` / `WaitIdle refused` across every instance of every re-measurement. That is the expected reading — the path needs an allocation refusal — and it is worth grepping for, because a run that *does* print them was refused its resources for real |

The whole world viewport is terrain ink (630 784 px at 1024×768; the 65 black pixels are the
frame's own), so this is emphatically not two blank frames agreeing — the failure mode
`tools/vk-ab.py` exists to name.

**It is not a second implementation of the pass:**

| | where it comes from |
|---|---|
| the instances | `s_inst` — the four-short-per-cell array the GL gather filled and the GL upload took, handed over **exactly once** |
| the unit quad | `TAGPU_TERR_QUAD` in `tagpu_terr.h`, **one literal both lanes build their per-vertex buffer from** |
| the uniforms | the numbers the GL draw passed to `uGame`, `uZoom`, `uZoomC`, `uDepthScale`, `uEnc`, `uOrigin`, `uTile0`, `uTexel`, `uRestored`, `uHDim`, `uFog*`, `uLit`, `uLambert`, `uSun`, `uAmb`, `uNorm`, `uShadowOn` |
| the tile atlas | `tagpu_terr.c`'s **CPU mirror** — the very buffer `glTexImage2D` was handed, kept instead of freed |
| the height grid | the same, for the R8 grid `build_height` uploads |
| the palette | `tagpu_pal_live()`, the buffer `s_palTex` is uploaded from |
| the fog grid | a **copy of** the packet's grid, taken at publish time into a buffer the publishing module owns — the same bytes the GL lane hands `glTexImage2D(GL_RG8, …)`, and the pointer is not the packet's. It was the packet's until the G19e landing review (2026-09-15) pointed out that `tagpu_packet_frame_end()` ends that pointer's declared lifetime *before* `render_ogl.c` runs the Vulkan lane |
| the fog shade LUT | `tagpu_native_foglut()` — the 256 bytes last uploaded to `s_fogLutTex` |
| the shader | `inc/spirv/tagpu_terr.spv.h`, generated from the GL strings by §2.25 — a 5840-word FS with a 192-byte uniform block and eight samplers, and a 765-word VS with a 64-byte one |

#### Instancing, and the one trap in it

The GL twin draws **six vertices and one instance per visible cell** (`glDrawArraysInstanced`,
`glVertexAttribDivisor(1, 1)`), which is what lets a 4K view at the zoom floor fit in a
megabyte. In Vulkan that is a second `VkVertexInputBindingDescription` at
`VK_VERTEX_INPUT_RATE_INSTANCE` and `vkCmdDraw`'s `instanceCount` — small, and new to this lane.

**The trap is the format.** The cell record is four **unnormalised `GL_SHORT`s** read into a
`vec4` attribute, so GL's fixed-function conversion is "the integer, as a float". The Vulkan
format that does that is **`R16G16B16A16_SSCALED`**; `_SINT` would require the shader's
attribute to be an `ivec4` and would read as garbage against a `vec4`. SSCALED is not a format
a driver must support as a vertex buffer, so `build_pipeline` **asks** for
`VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT` and the pass stays down if the answer is no — naming the
fallback (widening the record to four floats on the CPU, where every field is a small integer
and exact) rather than guessing. The 4070 takes it.

#### The texels, and why this pass needed no mirror mechanism

§2.29's GAF atlas is painted **incrementally**, which is why it needed a mirror that is correct
from the instant it exists. Terrain's two big textures are built **whole**, once per map, out of
a buffer that was freed three lines later — so the whole of the answer is *do not free it*:

> **`s_atlasMirror` and `s_hMirror` ARE the buffers** `glTexImage2D` was handed, kept rather
> than freed. Nothing writes either again — the tile set is built by `LoadMap` and never
> changes — so they cannot drift from the textures.

The flag that asks for them (`s_mirrorWant`, set on the GL twin's 30-frame arm poll when
`tagpu_vk_armed()`) has one job the GAF mirror's did not: **force one rebuild when the lane arms
after the texture was built.** `ensure_atlas` and `ensure_height` both early-return on an
identity test, and the extra term in each is what makes them fall through *exactly once* — the
only way to obtain a mirror for a texture that already exists is to build it again. Measured:
on an instance armed before launch the poll runs before the gather, so the **first** build
already retains, and the log shows one `terr: atlas built …` line and no
`no CPU mirror yet` refusal.

**What they cost, and it is per map, not per frame:** Two Continents' atlas is 2176×2720 for
5062 tiles = **5.64 MB**, its height grid 672×800 = **525 KB**; Anteer Strait's are 2176×3774
for 7051 tiles = **7.83 MB** and 578×584 = **330 KB**. Paid for only while the Vulkan lane is
armed, and once asked for, kept for the process's life — un-asking would buy back memory that a
re-arm spends again.

**The hills mesh is not mirrored and does not need to be.** `build_hills` is the heightfield
*caster* for `tagpu_shadow.c`, a VBO/IBO the terrain fragment shader never samples.

#### Two shared images whose dimensions change — the lifetime §2.28 named

Per-slot, the atlas would be eight copies of 5.6–7.8 MB in a 32-bit address space whose largest
free block is the number this phase spends its budget measuring. So both big images are
**one image each**, uploaded when the mirror's serial says the bytes moved, behind §2.29's
write-after-read barrier.

Unlike §2.29's atlas, **their dimensions genuinely change**: `ATLAS_DIM` there is a compile-time
constant, while terrain's atlas is `ceil(tiles/64) × 34` texels tall and the height grid is the
map in 16-px cells. A shared image that has to be *replaced* cannot have its descriptor
rewritten under frames in flight. Two things make that safe by construction:

* **Every slot's samplers are rewritten during THAT SLOT'S OWN `prepare`** (`shared_bind`),
  which is the one instant the seam's fence proves nothing of ours is in flight for it. No write
  in the file ever touches a set another frame may be using — which is the property §2.29 got by
  doing its one such write before any set had ever been bound, and cannot be had that way here.
* **The replaced image is retired behind a slot bitmask, not a timer.** `pending` starts as
  every slot; a bit clears when that slot has been **visited** — at the top of its own
  `prepare`, unconditionally. It does **not** clear "when that slot's set has been rewritten":
  this page and the file header both said so until the G19e landing review (2026-09-15) and
  both were wrong, because the bit has to clear on the paths that return *without* binding or
  the retire stalls (see the paragraph below). Two facts bound the old image's last reference:
  no *submitted* command buffer can still name it through a cleared slot, because that slot's
  submit has completed; and no *future* one will, because `record` runs only when `prepare`
  returned 1 and every such `prepare` calls `shared_bind` first. So `pending == 0` means
  unreferenced, whatever the frame rate and whatever the driver. **That second fact was an
  argument spread over two functions and a seam in another file; since the review it also carries
  an assertion**: `shared_bind` records the views it wrote into each slot
  (`boundAtlas`/`boundHeight`) and `record` draws nothing unless they are still the live ones, so
  a slot left holding a retired view cannot sample it even if some future path reaches `record`
  without rebinding. **It is not, however, "the fact that licenses the destroy" — the `pending`
  bitmask still is, and this page said otherwise until the re-review.** `prepare` has exactly one
  `return 1` with `shared_bind` on the straight-line path before it, so the comparison is a
  tautology on every path that exists today and **can never fire**. It is a guard against a future
  `prepare` that returns 1 without binding, which is worth its two lines; it is not evidence.

**The accounting is done first and unconditionally**, at the top of `prepare`, and that is not a
detail: doing it inside `shared_bind` stalls for ever on the one path that matters — a resize
that cannot be applied because a retire is outstanding returns *before* binding, so the bit that
would end the retire would never clear. A second change arriving while one is outstanding draws
**nothing** for the frames it takes to clear rather than starting a second retire; at most
`slots` frames, and only for back-to-back map changes.

**MEASURED, and it is the honest half of this: that retire is NOT reached by the game as it
stands.** A map change is the only thing that moves either dimension, and it goes through a
shell transition that stops the GL render thread — which brings the whole Vulkan lane down and
back up, this pass included. The in-process cycle run here (Two Continents → `MAINMENU` →
Anteer Strait, no relaunch) logs `vk: render thread stopping - down` twice, rebuilds the atlas
at 2176×3774 and the height grid at 578×584, and brings the pass back with
`vk: terr: the Vulkan edition is up`. So the retire is **the guard that keeps the code correct
rather than a path the game reaches today** — and the code cannot be written without it, because
the dimensions are data and cannot be asserted away.

#### Pipeline state: depth writes, no blending, and the scissor again

* **Depth is §2.28's answer, used unchanged**: `VK_COMPARE_OP_LESS`, and **writes ON** — terrain
  is the frame's implicit far plane and everything above it is tested against what it wrote, so
  the pass refuses to arm when `d->dfmt` is `VK_FORMAT_UNDEFINED` rather than drawing untested.
  The viewport carries `minDepth 0.5 / maxDepth 1.0`, which is GL's `(z+1)/2` exactly. One
  pipeline, not §2.29's two: there is no shadow bucket here.
* **No blending.** The GL twin draws terrain *before* `glEnable(GL_BLEND)` (`tagpu_native.c`),
  and terrain is opaque with alpha 1 everywhere. §2.29's premultiplied pair is its own.
* **The scissor is §2.29's**, mirrored as `offset.y = H − (vpT + vh)` with its enable travelling
  with it. It matters more here: terrain covers the whole *gather* rect, which at zoom < 1
  reaches well past the viewport and over the side panel.
* **Both halves were upside-down pictures of the world — true until landing 5b and no longer**;
  the flip that made the Vulkan half match the GL *FBO* also made Route D present the world upside
  down, and both halves are now the right way up. §2.40.

#### What it does not do

**Two surfaces of the GL lane have no CPU mirror, and the pass refuses a frame that would need
either** rather than draw a different picture from its own oracle:

* **`uRestored` 1** — Classic++'s restored tile atlas (`tagpu_restoreglsl.c` writes it on the GPU
  and it is never read back), which is §2.29's refusal again.
* **`uShadowOn` 1** — the Classic++ cast-shadow depth map (`tagpu_shadow.c`), a GL depth texture.
  Its two samplers must still be **valid** for the set to be bound, and a `sampler2DShadow` needs
  a real depth image and a compare-enabled sampler — so they name a **1×1 `D16_UNORM` image**
  this file makes and clears to 1.0 with `vkCmdClearDepthStencilImage`. `uAtlasRGB` is a
  placeholder too and names the atlas's own view, exactly as §2.29's binding 42.
  **[SUPERSEDED BY §2.32, 2026-09-15.]** The map is drawn by `tagpu_vk_shadow.c` now and this
  pass samples it; the dummy is named only on a frame with no map, and the compare sampler is
  LINEAR rather than NEAREST because the twin's PCF is. What is left of this refusal is
  per-frame: a frame the shadow pass could not reproduce, which is every frame with a unit
  caster until the unit pass lands.

Both were Classic++ surfaces, so a `tacli` instance — which opts out of the play defaults — never
met either; a `--defaults` instance does.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes —
so the barriers, the stage masks and the layout transitions are argued from the specification and
from a correct picture. Also not covered: any resolution but 1024×768 and 1920×1080, any device
but the 4070, Windows, `ss` 2, Classic++ (both refusals above), **the retire firing** — the
one path in this file no run has watched execute, for the reason measured above — and, since the
landing review, **the owed teardown firing**, which is the same kind of gap for the same kind of
reason: it needs the device to refuse a slot its resources, and the 4070 with 247 MB of largest
free block does not. Both are guards that keep the code correct rather than paths the game
reaches today, and neither can be asserted away, because in both cases the trigger is data.

#### What the landing review changed, 2026-09-15

Two reviewers read `main...HEAD` independently at `high`, one on correctness and one on
synchronisation alone. **Both returned the same first finding**, and it was real. The fixes below
are on the branch, and **they have now been RUN**: every A/B, the regressions and constraint 4
were re-measured on the reworked binary and all of them reproduced — the table above is that
re-measurement, not the pre-rework one. The rework's own review — which `CLAUDE.md` asks for when a
fix changes the synchronisation *design* rather than patching it, and this one moves a teardown
across a submit boundary and adds a drain point to the seam's frame loop — **has since run, and it
found a blocker**: it is the next subsection, and the table above is the *third* measurement, taken
after its fixes.

**What the re-measurement does and does not cover.** It exercises every path the rework touched
*except the refusal itself*: the publish gate (every shipped frame in constraint 4 runs with the
lane down), the copied fog grid (every terrain frame), the `record` bound-views test (every
terrain frame, and it would show as a dropped draw over the whole viewport), the `_down`
state change (the clear-and-re-arm cycle), and the A/B write guard (every capture). **The owed
teardown itself is still unfired** — it needs the device to refuse a slot its resources, which is
the same reason §2.28's uncovered list has stood since the scaffold. So the drain path in
`tagpu_vk.c` is argued and reviewed, not run, and that is the honest statement of it.

* **A pass may no longer tear itself down mid-frame.** `prepare`'s refusal path — reached when
  the device will not give a slot its buffers or images, which is the 32-bit address-space
  pressure this phase exists to measure — used to call `tagpu_vk_*_down()` on the spot. That
  destroys the pipeline, the descriptor pool, the shared images and **every** slot's buffers,
  while the seam has waited on `fence[slot]` **alone**: every other slot's submit is still
  executing against them. Worse, the command buffer of the frame in hand has already had
  `shadow_ready`'s barrier and depth clear — and the atlas uploads — recorded into it, and
  `vk_present` submits it whether the pass draws or not. **It fires on the first refusal, not
  rarely.** The pass now stops drawing and raises `s_downOwed`; `tagpu_vk.c` checks
  `tagpu_vk_{terr,feat,scaffold}_down_owed()` at the top of the next frame — before `cb` is
  reset, so nothing names the objects yet — runs `vkDeviceWaitIdle`, and only then tears the
  pass down. An unresolved `vkDeviceWaitIdle` is **not** treated as idle: with no way to prove
  the device quiet the lane comes down instead. The `build()`-failure teardowns were left alone;
  they run with the pass `ST_UNBUILT`, so nothing of it is in flight.
* **The retire's stated invariant was wrong, and is now also a test** — see the bullet above.
* **The fog grid is copied at publish time** instead of handed over as a pointer into a
  frame-packet slot, whose declared lifetime (`tagpu_packet.h`) ends at
  `tagpu_packet_frame_end()` — which `render_ogl.c:1623` calls before it runs the Vulkan lane at
  `:1674`. It held only because the give-back happens at the next acquire; that is also where
  the `poison` lever fills the slot, so this was the one stale read that lever could not see.
* **The A/B no longer claims a Vulkan half on a GL half that was never written.**
  `tagpu_abshot_end` now returns whether the capture reached the disk, and the four callers set
  `s_abFrame` from it. `tagpu_fps.c` had this guard (`can`) on `main`; extracting the shared
  module dropped it, and the cost was `vk-ab.py` diffing a fresh Vulkan capture against a
  **stale** `_gl.ppm` from an earlier run and reporting a port failure.
* **Nothing is published on a shipped frame.** `terr_publish` and `feat_publish` ran their
  `memset` and ~40 stores on every frame with the lane down; they now return at once unless the
  module's mirror latch is set.
* **The cell count is bounded in the pass**, like the atlas, height and fog dimensions already
  were — it sizes both the instance buffer and `vkCmdDraw`'s `instanceCount`. The bound is the
  producer's own 24 MB clamp restated in this file's terms, deliberately **not** tighter: a pass
  that refused a cell count the GL twin drew would report a rasteriser difference over the whole
  viewport.

**Left unfixed, deliberately.** Arming the lane forces one height-grid rebuild
(`ensure_height`'s mirror term), and `build_height` zeroes `s_hW` and frees the mirror before it
starts — so if that one rebuild fails the *GL* lane draws unlit for up to 60 frames where it
previously kept a working texture. It needs `tagpu_vk.on`, and it needs a grid that was readable
a moment earlier to fail `ptr_ok`/`IsBadReadPtr`. Restructuring `build_height` to commit only on
success would break the "`s_hMirror` non-NULL and it is `s_hW × s_hH`" single-fact invariant that
function is written around, which is not worth doing inside a landing already carrying a
synchronisation rework.

#### What the RE-review changed, 2026-09-15

The rework above changes the synchronisation *design* rather than patching it — it moves a
teardown across a submit boundary and adds a drain point to the seam's frame loop — so `CLAUDE.md`
asks for a second review, and two more reviewers read `main...HEAD` at `high`, one on correctness
and one on synchronisation and object lifetime alone. **Both led with the same finding again**,
independently, exactly as the first pair did. Every finding below was verified against the code
before anything was changed.

* **THE BLOCKER, AND IT IS THE FIRST REVIEW'S BUG IN A SECOND PLACE.** The terrain hand-over could
  **outlive the frame that published it, and its pointers could be freed underneath it.**
  `s_pubHave` was cleared only inside `tagpu_terr_render`, which `tagpu_native.c` calls only when
  the gather returned cells — so every `terr_bail()` exit left the previous frame's hand-over
  standing, and `tagpu_terr_glreset` did not clear it either. Meanwhile `ensure_atlas` **frees**
  `s_atlasMirror` and `build_height` frees `s_hMirror` on a map change. The sequence is short and
  is not exotic: publish on a frame the lane is too young to consume, change level, the gather
  bails at `ptr_ok(tmap)` *during the load* — which is the same event that rebuilt the atlas — and
  the lane's next `prepare` `memcpy`s up to 5.9 MB **out of freed heap**. The non-crashing version
  of it is last frame's terrain drawn over this one, which `tagpu_terr.h` said was impossible.
  What makes it conclusive rather than theoretical is that **the codebase already knew the rule in
  two of its three places**: `tagpu_feat_glreset` clears the flag with a comment explaining this
  hazard, and `tagpu_scaffold_frame` clears unconditionally at the top of the frame. The terrain
  pass was the odd one out, and the asymmetry was sitting in the diff.
  **The fix is a bound, not an enumeration.** The hand-over now carries the frame it was published
  on and `tagpu_terr_handover`/`tagpu_feat_handover` refuse any other, so "these pointers are
  alive" is a property of the frame number rather than of which functions happened to run — a
  future `terr_bail` that forgets cannot resurrect the bug. The flag is *also* cleared on the bail
  paths and in `tagpu_terr_glreset`, so the flag tells the truth as well. The frame number reaches
  a pass as `TAGPU_VKPASS::frame`, from `tagpu_vk_frame`, from `render_ogl.c`'s own counter — the
  same number the GL lane stamped with earlier in that iteration.
* **A teardown nobody asked for could latch the pass refused for the life of the process.**
  `_down` read `s_downOwed` directly and ended at `ST_REFUSED` when it was set — but `vk_down` and
  `vk_resize` call `_down` for their own reasons, and `prepare` treats `ST_REFUSED` as terminal.
  So one transient refusal followed by a window drag (the acquire returns `OUT_OF_DATE`, the
  caller resizes before the seam's drain can run) killed the pass **permanently**, on a device
  that was then destroyed and replaced anyway. This was a regression the rework introduced: before
  it, `_down` always returned `ST_UNBUILT` and the pass recovered on the next lane cycle. The debt
  is now settled only by the seam's own `tagpu_vk_*_down_paid`, called after its `vkDeviceWaitIdle`;
  every other caller of `_down` leaves the debt standing and returns the pass `ST_UNBUILT`.
  **The re-arm measurement in the table above could not have caught this** — it exercises the
  non-owed branch — and the table now says so.
* **A failed fog copy published `fog` 1 with no grid.** `realloc` can refuse, which is precisely
  the address-space pressure this phase exists to measure, and the pass then sampled a 1×1 image
  while `uFogDim` carried the real dimensions: the GL twin draws correct fog, the port draws
  something else, in silence. Clearing `fog` instead would be just as silent a difference the
  other way, so the publisher now **publishes nothing at all** for that frame — the answer this
  file already gives for the restored atlas and the shadow map. Both publishers.
* **`tools/vk-ab.py` compared whatever files existed.** Existence is not freshness: every way a
  capture silently does not fire — the `.ab` lever not re-armed (`touch` on an existing file does
  nothing), the lane down, `ss != 1`, the seam's "N levers claimed this frame" refusal — leaves
  the previous run's PPMs on the disk, and the tool would print **that** run's verdict for the
  binary in front of you. **The whole of this landing's evidence is that number.** It now refuses
  a capture older than the `.ab` file that asked for it. (The re-measurements in the table were
  taken by a harness that deleted the PPMs before every capture, so a missing half would have been
  reported as missing; the tool no longer depends on the operator remembering that.)
* **`_down`'s early return** (`!dev || !vkDestroyBuffer`) discarded the verdict along with the
  debt. Neither reviewer could reach it and neither can I — `owed` implies the pass was `ST_READY`,
  which implies a live device — but it now latches `ST_REFUSED` for the same reason the path below
  it does.

**And four documentation claims the code disproves**, which is the half of a review this project
counts as findings rather than polish:

* **The `boundAtlas`/`boundHeight` test in `record` is inert — it can never fire**, because
  `prepare` has exactly one `return 1` and `shared_bind` is on the straight-line path before it.
  It is a guard against a future `prepare` that returns 1 without binding, and worth its two
  lines; it is **not** "the fact that licenses the destroy", which is what this page, the file
  header and `fecd146`'s own commit message all called it. The `pending` bitmask still is.
* §2.28's "loses 117 lines (501 → 384)" was never re-taken after `fecd146` added six lines:
  **111, 501 → 390**. The commit that re-measured this landing said "every number reproduced" and
  vouched for it without counting.
* The **ta-drive** skill told an operator to look for `depth format 0` on a device with no 24-bit
  depth. That line cannot be printed — the pass returns before it — and what does print is
  `the seam's render pass carries no depth attachment …`.
* The re-arm row of the table above read as though it validated the owed branch of `_down`. It
  validates the other one.

**What the reviewers checked and found clean** is worth recording too, because it is most of the
lane: the retire bitmask's invariant holds (bits clear under each slot's own fence; a second
resize is refused while one is outstanding; `slot_free` clears the bound record); no descriptor
set is written outside its own fenced `prepare`; the drain point really is before anything names a
pass's objects, and the debt is always settled and never twice; `CELL_MAX` equals the producer's
own clamp exactly and `HEIGHT_MAXDIM`/`FOG_MAXDIM` match theirs, none tighter; the GAF mirror
copies the rows `glTexSubImage2D` gets; and **no GL pixel moves on the unarmed path**. Nothing in
the rework rests on timing — the drain is a real `vkDeviceWaitIdle`, the retire is a bitmask under
fences, and no `IsBadReadPtr`, sleep or retry was added. The gaps were missing bounds, not weak
ones.

### 2.31 The effects pass, drawn by Vulkan (`tagpu_vk_fx.c`, `tagpu_fx.c`'s four buckets) — Phase G, G19e

The **fourth world pass**, the first that needs **more than one pipeline for one shader**, and
the first whose parity is not closed by construction — because it is the first that draws
**lines**.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_fx_handover`, so there is nothing for [exe reverse engineering](exe-reverse-engineering.html)
in this landing and nothing for the hook map above. `tagpu_vk_fx.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1`, 1024×768, on the
binary this landing builds and with the tree clean at that commit — which is the check that
matters, because the link is not byte-reproducible (§2.30).

| | |
|---|---|
| **triangles only** (`fx.on=nolines`), `fx-mix`, three runs | **0 of 786 432** every run, **5704 / 4061 / 23 932** non-black each side |
| **all four buckets**, `fx-mix`, three runs | **0 / 2 / 0**, ink 5008 / 3970 / 5812 each side |
| **all four buckets**, `fx-lasers`, three runs | **3 / 3 / 2**, ink 897 / 900 / 2835 each side |
| **lines only**, `fx-lasers`, four runs | **3 / 0 / 0 / 6**, ink 100 / 50 / 26 / 152 each side |
| constraint 4 | **0 of 630 784** against `main` with `tagpu_vk.off`, over a floor measured at **0** — see below, because the floor is the part that needed care |

So **everything this pass draws as triangles is exact** — the weapon sprites, the explosion
flashes with their additive blending, the debris, all ten particle layers, the palette lookups,
the fog and the depth test, at **35 707 ink pixels a side** on the busiest frame measured. What is
not exact is the **line** bucket, and the rest of this section is why.

#### Three pipelines for one shader, and why that is new

The GL twin draws four buckets with one program and three pieces of fixed-function state
(`tagpu_fx.c`, the draw at the end of `tagpu_fx_render`):

| bucket | topology | blend | what it is |
|---|---|---|---|
| `B_UNDER` | `TRIANGLES` | `ONE / ONE_MINUS_SRC_ALPHA` | particle layers 0..6 — wake foam, feature smoke, trail puffs, nanolathe |
| `B_LINES` | **`LINES`** | the same | lasers (RenderType 0) and lightning (RenderType 7) |
| `B_FLASH` | `TRIANGLES` | **`ONE / ONE`, additive** | the LHT explosion flash |
| `B_SPRITES` | `TRIANGLES` | back to the first | weapon sprites, explosions, particle layers 7..9 |

Topology and blend are both baked into a `VkPipeline`, so that is **three objects off one
layout** — the feature pass needed two for its depth-write modes and this is the same shape one
step further. `VK_EXT_extended_dynamic_state3` would make the blend dynamic and buy exactly one
object; it is not worth an extension. Depth is **tested and never written** for all three: the
native pass leaves `GL_LESS` and `GL_DEPTH_TEST` standing around this draw and the twin brackets
itself in `glDepthMask(GL_FALSE)`, so effects occlude nothing and are occluded by everything drawn
before them.

#### The line rule, found twice — one fixed and one open

**FIXED, and it is a named difference with a named fix.** Vulkan's default
`lineRasterizationMode` drew a **strict superset** of the twin: all 126 of its pixels plus
**exactly one extra fragment at the END of each segment** (4 px on the fixture, GL 126 ink against
Vulkan 130, with *no* GL-only pixel anywhere). GL's non-antialiased lines follow the **diamond-exit
rule** and Vulkan's default mode does not; `VK_LINE_RASTERIZATION_MODE_BRESENHAM` does. The seam
now asks for `VK_EXT_line_rasterization` and enables **`bresenhamLines`**, publishing it to passes
as `TAGPU_VKPASS::lineok`, the same shape as `flipok`. A pass with line vertices and no `lineok`
refuses the frame. That took 4 px to 0.

**The test for it is a QUERY, and this file said otherwise until the landing review.** It claimed
that enabling the feature made `vkCreateDevice` the test, because a device that lacks a requested
feature must fail with `VK_ERROR_FEATURE_NOT_PRESENT`. That does not hold here, and **both**
reviewers found it independently: this instance is Vulkan 1.0 and did not enable
`VK_KHR_get_physical_device_properties2`, which `VK_EXT_line_rasterization` depends on — so an ICD
that does not consider the extension properly enabled is entitled to **ignore the unrecognised
`pNext` struct and return `VK_SUCCESS`**, which is indistinguishable from having enabled it. The
pass would then build its line pipeline with `BRESENHAM` chained on a device where the feature is
off: undefined behaviour rather than an error, and in practice the default mode — the four-pixel
superset this was added to remove, back again with nothing in the log. The seam now **asks the
instance for that dependency when the loader offers it, and asks the DEVICE for the feature bit**
through `vkGetPhysicalDeviceFeatures2KHR` before setting `lineok` at all; no query, no lines.

**OPEN, AND IT IS STRUCTURAL: THE Y FLIP AND EXACT LINE RASTERISATION ARE IN TENSION.** What is
left with Bresenham on both sides is a **one-row shift of a whole segment at a single column**,
deterministic to the pixel — the same 3 px on every run of the same geometry. The measured case:
one laser bolt, **x 553..602 (dx 49) by y 410..430 (dy 20)**, two parallel lines of different
palette colours; **49 of its 50 columns are identical**, and at x 578 the GL pair sits at rows
{420, 421} and the Vulkan pair at {419, 420}.

The reason is the flip, and it is worth stating carefully because every remaining pass that draws
lines inherits it:

* A **triangle** is filled by a coverage test each pixel centre passes or fails on its own. That
  test is symmetric under reflection, so mirroring the raster grid mirrors the result exactly —
  which is why every world pass so far reads 0 and why the triangle buckets above do.
* A **line** has no inside. It is *walked*: step a column, round to the nearer row, step again.
  Where the exact line passes **exactly halfway** between two rows the walk needs a tie-break, and
  the rule — in both APIs — rounds toward larger `y` **in the space the rasteriser is working in**.
* The Vulkan lane rasterises the frame **mirrored** (the negative viewport height, which is how
  every ported shader keeps GL's clip convention without a source edit). Mirroring is exact: a
  pixel centre maps to a pixel centre and an endpoint to an endpoint, nothing lands between. But
  "round toward larger `y`" in mirrored space is "round toward **smaller** `y`" in the world. So at
  a tie, **and only at a tie**, the two lanes choose opposite pixels.

That is why it is rare (one tie in fifty columns on this bolt, none at all on the shorter ones)
and why it is perfectly reproducible when it happens.

**THE BAR IS ONE PIXEL PER TIED SEGMENT, NOT A FIXED THREE**, and the table above is what that
looks like. A segment that hits a tie loses one pixel and gains one: **2 px** for a bolt drawn as a
single line, **3 px** for one drawn as two parallel lines (the pixel between the lost and gained
rows also changes which of the bolt's two colours wins there), and they ADD — **6 px** was measured
on a frame with two tied bolts. So the honest statement of the bar is *a couple of pixels per laser
that happens to land halfway*, with **6 of 786 432 the worst seen** across every run taken here, and
not a constant. A frame full of lasers has more segments and therefore more chances to tie; nothing
measured here bounds it at six. **There is no fix inside the current
design**: pre-mirroring the line geometry, or flipping in the shader, would mean the Vulkan lane
draws from something other than what the GL lane drew, which is the one thing the oracle forbids.
**The owner's decision, 2026-09-15, is to ship it as a stated bar** — a couple of pixels per tied
laser, 6 of 786 432 at the worst measured, on bolts that move every frame — rather than refuse line
frames. It is the
first time this lane claims a bar where an exact oracle *does* exist, and it is recorded here
because the unit pass's wireframe bucket (`tagpu_posedraw.c`'s `TAGPU_PB_WIRE`) and the hi-res
path draw lines too and will inherit exactly this.

#### A three-byte format Vulkan does not have

The flash light table is `glTexImage2D(GL_RGB8, 32, 1, …)` on the GL side, and
**`VK_FORMAT_R8G8B8_UNORM` is optional and not supported for sampled images** on the device this
lane is measured on. The 96 bytes are therefore **expanded to 32 × 1 RGBA8** on the way into the
staging buffer, alpha 255; the shader reads `.rgb`, so the byte it gains is never looked at.
`build_sampler` asks for every format this pass samples **by name** and refuses the pass rather
than discover it later — a format the GL lane takes for granted and the Vulkan lane has to ask for
is exactly the kind of thing that is invisible until a picture is wrong.

#### It is not a second implementation of the pass

| | where it comes from |
|---|---|
| the vertices | `s_verts[4]` — the four bucket arrays the GL gather filled and the GL upload took, handed over **exactly once**, concatenated in the twin's own draw order |
| the vertex layout | `TAGPU_FX_ATTRS` in `tagpu_fx.h`, **one table both lanes build from** — the GL VAO and the `VkVertexInputAttributeDescription` array |
| the uniforms | the numbers the GL draw passed to `uGame`, `uZoom`, `uZoomC`, `uDepthScale`, `uFog`, `uFogOrg`, `uFogDim`, `uScafP`, `uSS`, `uZoomF`, `uZoomCF`, `uRestored` |
| the sprite atlas | `tagpu_gaf.c`'s **CPU mirror** of the fx atlas, §2.29's mechanism, asked for on the arm beat only while the Vulkan lane is armed |
| the palette | `tagpu_pal_live()`, the buffer `s_palTex` is uploaded from |
| the flash light table | `s_lhtRGB`, the very buffer `glTexImage2D` was handed, moved to file scope for this |
| the fog grid | a **copy of** the packet's grid, taken at publish time into a buffer the publishing module owns — never the packet's pointer, whose declared lifetime ends at `tagpu_packet_frame_end()` |
| the fog shade LUT | `tagpu_native_foglut()` |
| the shader | `inc/spirv/tagpu_fx.spv.h`, generated from the GL strings by §2.25 |

#### What it does not do

* **THE SCAFFOLD TEST.** `uScafOn` is 1 for the `B_UNDER` draw alone when the G12a scene-depth
  scaffold is armed, and the fragment shader then samples `uScaf` — **which is another pass's
  texture**. `tagpu_vk_scaffold.c` holds those texels in an image it owns privately and
  `tagpu_vk_scaffold.h` exposes no view; sharing one image between two passes is a mechanism with
  an ordering contract of its own and this landing does not build it. A frame whose twin had the
  test on is **refused**, said once. The scaffold is a debug overlay and is not in the default arm
  set, so the measured configuration is unaffected — but **the unit pass and the hi-res path
  sample the same texture** (`tagpu_native.c:3834, 3994, 4245, 4303`), so whichever landing ports
  those has to answer it, and the answer should be chosen once for all three.
* **Classic++'s restored atlas**, as in every world pass: no CPU mirror, so a frame whose twin
  reported `uRestored` 1 draws nothing and says so once.
* **The effects MODELS.** RenderType 1/3/6 projectiles are emitted as 3DO nodes and drawn by
  `tagpu_native.c`'s unit pipeline, not by `tagpu_fx.c`'s own program — so they are the unit
  pass's to port, and the A/B's clear erases them from the GL half exactly as it erases the
  terrain.
* **`ss` 2.** `glLineWidth(ss)` makes the twin's lasers two pixels wide, and a Vulkan `lineWidth`
  other than 1.0 needs the `wideLines` device feature, which is the seam's to enable and it does
  not. A frame with line vertices at `ss != 1` is refused, naming the cause. (The A/B needs `ss=1`
  anyway: the GL capture is the supersampled FBO and the Vulkan one is the client rect.)
* **The validation layer**, still — none is installed in the wine prefixes, so the barriers and
  stage masks are argued from the specification and a correct picture.
* **The owed teardown**, still unfired on any instance, as in every pass since the rework.

#### Constraint 4, measured — and the floor artefact is the PAUSED banner, blinking

**0 differing pixels of 630 784** in the world viewport (`vp=(128,32,896x704)`), this landing's
DLL against `main`'s (`e39b762`), both with `tagpu_vk.off` and the full arm set, on
`selbox-facings` at 1024×768, pointer parked in the side panel, sim paused — **on two independent
pairs**, and **0 for every other pairing taken**, including two separate relaunches of the same
binary. **Re-measured on the binary the landing review's fixes produced and still 0**, which is the
whole table's rule here: those fixes changed device creation, so every figure above was taken
again on the binary that would land rather than carried over.

**The artefact that looked like a floor is the engine's own `PAUSED` banner, and it BLINKS.** Two
paused instances captured at different moments in that blink differ by the whole banner:
**2 747 px** on one pair, **2 933 px** on another, in a band at **x 502..636, y 372..400** — which
is exactly where the word sits at this resolution. Every one of those pixels is inside it and
**none is outside**:

| pair | differing px | outside the banner |
|---|---|---|
| landing vs `main`, pair 1 | 0 | **0** |
| landing vs `main`, pair 2 | 2 747 | **0** |
| landing vs landing (same binary, two instances) | 0 | **0** |
| landing vs itself, separately relaunched | 2 933 | **0** |
| `main` vs landing, separately relaunched | 2 933 | **0** |

So the GL lane did not move, and the way to see that is to **exclude the banner rect or catch both
halves in the same blink phase** — not to load and pause the two instances together, which is what
this section said until the crop was actually looked at. Loading together changes nothing: the
blink is free-running, and a simultaneous pair lands in phase or out of it by luck. *(The first
pair here read 0 and the inference drawn from it — tick skew, fixed by pausing together — was
wrong; cropping the 180×60 region and rendering it showed the word `PAUSED` in one capture and
bare grass in the other. §2.28's rule to measure the floor every time is what surfaced it, and
looking at the picture rather than the number is what explained it.)*

This is a **different artefact from §2.28's two-state 71-px sliver** at x 1017..1023, y 236..277,
which is at the frame's right edge and is not the banner. Both are reasons the floor is measured
rather than quoted.

### 2.32 The cast-shadow depth map, drawn by Vulkan (`tagpu_vk_shadow.c`, `tagpu_terr.c`'s caster mirror) — Phase G, G19e

The **fifth world pass**, and the first that **draws into something other than the frame**. Every
pass before it records into the seam's render pass and produces pixels; this one owns an offscreen
depth image, draws the casters into it from the light's point of view, and leaves it in
`SHADER_READ_ONLY_OPTIMAL` for the passes that sample it. `tagpu_vk_terr.c` stops refusing
`uShadowOn` frames and samples it, which closes an uncovered case §2.30 had on record.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_shadow_handover`, so there is nothing for
[exe reverse engineering](exe-reverse-engineering.html) in this landing and nothing for the hook
map above. The one engine word this work reads at all is `main+0x37F06` bit 2, the Shadows option
the GL twin already gates on, and it is already recorded there. `tagpu_vk_shadow.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1`, 1024×768, **on
the binary the landing review's fixes produced** — every figure below was taken twice, once before
the review and once after, and the second table is the one that stands. (The first run's ink counts
were a few hundred higher because the camera was a pinned one rather than the scenario's own; the
verdicts were the same.)

| | |
|---|---|
| **a heavily shadowed frame** — `static-terrain`, Classic++ `assets=0 shadows=1 terrainshadow=1 shadowsun=225,8`, where turning the map off moves **529 130** of the frame's pixels | **0 of 786 432**, **630 458** non-black each side, reproduced |
| the same, **`light=0`** (the lambert off, the map still on) | **0 of 786 432** |
| an **empty** map — `terrainshadow=0`, which is the Classic++ **default** | **2 of 786 432**, and they are the lambert's (below) |
| the default sun `225,40`, `terrainshadow=1` | **2 of 786 432**, same pixel every run, three runs |
| the control: `shadows=0` (no map at all) | **the same 2 px**, at the same pixel |
| the control: `light=0 shadows=0` | **0 of 786 432** |
| the **refusal** with one posed caster on screen | fires, says so once, the terrain pass stands down with it, and both recover to **0 px** (630 574 ink) when the caster leaves view |
| constraint 4 | **0 of 630 784** against `main` (`2e552d1`) with `tagpu_vk.off`, on **two** pairings, over a cross-launch floor measured at **0** |

**AND THE CASTER-MESH RETIRE HAS ACTUALLY FIRED**, which is the first time a retire in this lane
has: §2.30's shared-image one is still argued rather than run, because only a map change moves it
and that brings the whole lane down. This one is moved by a **live knob** — `terrainshadow` back to
0 gives the buffers back through the retire, and turning it on again logs a second
`caster mesh uploaded`. The **free** is what the next frame proves, not the upload: `mesh_resize`
returns −1 while `oldV` is still set, so a pass that drew 0 px with full ink after the cycle is a
pass whose `pending` bitmask reached 0 and whose `kill_buffer` ran.

So the depth map itself is **exact**: its geometry, its stored depth values, the 16 Poisson taps on
the raw depths, the blocker search, the bilinear PCF through the compare sampler and the
receiver-plane bias all reproduce the GL twin bit for bit over a frame two thirds of which is
shadow. The 1–2 px that remain are **not the shadow pass** — see "The lambert is 1 px, and it is
not this landing's" below.

#### It owns a second render target, and that is not a breach of the seam

Standing constraint 3 says surface, swapchain, acquire and present live in `tagpu_vk.c` and that
nothing else may know a window exists. **A render pass and a framebuffer over an image this file
allocated name no window**, so the second target lives in the pass rather than in the seam: the
seam still owns the one image that reaches a screen, and the pass would work unchanged against an
offscreen frame or another process's. That also keeps the seam's file from growing a second
personality every time a pass wants somewhere to draw.

What it costs is that the pass records a **whole render pass inside `prepare`** — begin, draw, end
— which is legal precisely because `prepare` is the hook the seam calls *outside* its own
`vkCmdBeginRenderPass`, and render passes may not nest. The two-phase contract was written for
texture uploads (a transfer may not be recorded inside a render pass); a second render pass turns
out to need exactly the same hook for exactly the same reason.

So the pass has a `prepare` and **no `record`**, and the seam does **not** add it to the `ndraw` /
`nclaim` counts. Those two exist to catch two passes contaminating one A/B capture; this one puts
no pixel in the frame, so counting it would refuse every capture taken while Classic++ shadows are
on — which is the configuration the shadow work is measured in.

#### GL's clip-space z range, and this is the pass that needed it

§2.29 item 1 settled the depth question for every pass that came after it: GL maps clip z in
[-1, 1] onto the depth range and Vulkan takes [0, 1], so a viewport with `minDepth 0.5 /
maxDepth 1.0` reproduces GL's `(z+1)/2` exactly, values and precision included, and
`VK_EXT_depth_clip_control` "is the answer only if a shader is ever found writing a z below 0."

**The shadow matrix is that shader.** `tagpu_shadow.c`'s `mrow` builds an orthographic projection
that fills [-1, 1] **by construction** — `ndc = 2(a·W − lo)/s − 1` over the light-space extent —
so under Vulkan's own convention the near half of every caster is CLIPPED AWAY and the map is
wrong rather than merely offset. And the values matter as much as the geometry: `taShadowAt`
compares `p.z * 0.5 + 0.5`, computed in the consumer's own shader, against what is STORED in the
map, so the viewport transform has to be GL's arithmetic and the format has to be GL's
quantisation.

The seam therefore queries `depthClipControl` and publishes it as `TAGPU_VKPASS::zclipok`, in the
same shape as `flipok` and `lineok`, and the pass stands down without it. **The feature is
QUERIED through `vkGetPhysicalDeviceFeatures2KHR`, never inferred from a `vkCreateDevice` that
succeeded** — the effects review's rule (§2.31), applied on the first day rather than after a
reviewer found it — and the device-creation ladder gains its own rung: depth clip control is
dropped first because it costs one pass, then line rasterisation, then the flip, and each rung
rebuilds both the extension list and the `pNext` chain rather than unlinking one struct out of
the middle of it.

**Measured on the reference setup, 2026-09-15**, with `tools/vkprobe.c` extended to print every
extension: the 4070 offers `VK_EXT_depth_clip_control` from a 32-bit process under **system wine
9.0**, and the device answers `depthClipControl` true. Two numbers worth recording beside it,
because they are not the ones §2.24 has: this wine enumerates **186** device extensions where the
Proton 11 run recorded 247, and neither list carries the four ray-tracing ones at 32-bit.

#### There is no Y flip here, and that is not an omission

Every pass that is **presented and writes GL's window convention** flips — a negative viewport
height, `VK_KHR_maintenance1` — because the two APIs disagree about which row of a window is the
top. Since landing 5b that is `gui`, `fps` and `scaffold` only: the world passes write the engine's
screen-space y, which grows downward, so clip −1 is already the game frame's top row and they take a
**positive** height. This pass was right for a third reason and still is.

**This target is sampled.** In GL, clip y = −1 is window row 0, which is texel row 0, which is
v = 0. In Vulkan with a *positive* viewport height, clip y = −1 is framebuffer row 0, which is
texel row 0, which is v = 0. The two agree already, and flipping would put every shadow in the
wrong half of the map. **The flip is a property of presentation, not of Vulkan** — which is worth
stating plainly, because "every pass flips" was on its way to becoming a rule of this lane rather
than a consequence of what each pass draws into.

Cull is off on both sides (`tagpu_shadow.c` disables `GL_CULL_FACE`, so a caster's back faces
write depth too), so the winding a flip would also have inverted is not in play either.

#### One map per frame slot; the caster mesh is shared and takes the retire

The map is written every frame and sampled in the **same** frame by the consumers, so with frames
in flight one image would have frame N's writes racing frame N−1's reads. **One image per slot**
makes the seam's fence the whole argument, exactly as the scaffold's per-slot upload does — and it
means a resolution change (the zoom octave moves the map between 256 and 4096) is a rebuild of the
slot we are being handed, under its own fence, rather than a retire. At the default `shadowres`
2048 that is 16 MB a slot and 64 MB at the ceiling, none of it allocated until a map is drawn.

The **caster mesh** is the other way round: it is shared by every slot and its size is data, so it
takes §2.30's slot-bitmask retire unchanged — the old buffers are held until every slot has passed
through its own `prepare` once, and `pending == 0` is then "no submitted command buffer names them
and no future one will", by construction. The only difference from the terrain pass's is that a
buffer is named by the **command buffer** rather than through a descriptor set, which changes
nothing about the argument. The accounting is done **first** in `prepare`, before every early
return, because the path that returns early is exactly the path that must still clear its bit.

`mesh_resize` returns **three** values rather than two — 1 built, −1 a retire still clearing, 0 the
device refused — because a refusal that read as "a retire is clearing" would loop silently for
ever, which is the shape §2.30's own `shared_resize` guards against with its
"is there an image at all" test.

#### The texels: the mirror is the buffer, again

`tagpu_terr.c`'s `build_hills` builds the heightfield caster once per map — one world-space vertex
per 16-px grid point, two triangles per cell, indices ordered by cell row so the rows under the
light window are one contiguous range — and hands both arrays to `glBufferData`. Since this
landing it **keeps** them while the Vulkan lane is armed (`s_mirrorWant`, the same latch the tile
atlas and the height grid use) instead of freeing them, so the Vulkan pass draws the same vertices
and the same index order rather than a second evaluation of that arithmetic. On Town & Country
that is **291 600 vertices and 1 743 126 indices, 10 226 KB**, uploaded once on the serial.

`tagpu_terr_hills_draw` also reports **the index range it actually drew**, clamped, so the Vulkan
pass draws the same `firstIndex`/`indexCount` rather than re-deriving the row clamp — the port
rule ("the port must not re-derive the pass's inputs") applied to a draw range rather than to
texels.

#### Only the heightfield casts, and a map missing a caster is a different map

The GL map is drawn from **four** kinds of geometry: the native 3DO stream, the posed program's
depth twin, the replacement meshes, and the heightfield. Only the heightfield has a CPU mirror on
this side of the seam today; the other three are the **unit pass's** to port.

So `tagpu_shadow.c` **counts** the casters it drew that the hand-over carries no copy of, and a
non-zero count is a **refusal** rather than a best effort. An OVER-count is the safe direction: it
refuses a frame the lane could have drawn, where an under-count would draw a different picture from
its own oracle. Measured: one posed caster on screen refuses the map, the terrain pass stands down
with it and says so, and both come back to 0 px the moment the caster leaves view.

**The census covers all FOUR kinds, and it did not at first — both reviewers led with that,
independently.** The native stream counts itself inside `tagpu_shadow_unit`; `tagpu_native.c`
reports the posed and hi-res ones the same way both loops draw them (`!castSkip`); and **the
heightfield counts itself too**, because `tagpu_terr_hills_draw` returns 1 whenever it issued the
draw and fills its out-parameter only when the mirror is there. "Drew, no mirror" therefore arrived
at the hand-over as a zeroed struct — **byte-identical to "the hills did not draw"** — which this
pass reads as an empty map and reports as complete, so the terrain pass would sample an all-1.0 map
while the GL twin's held the whole heightfield, and the A/B would call that parity. It is
reachable: `build_hills`' out-of-memory exit returns before it touches `s_hMeshW`/`s_hMeshH` or the
GL buffers, so a same-grid mesh from an earlier build keeps drawing while `s_hMeshNoMirror` stops
`ensure_height` ever asking again. `tagpu_shadow_hills` now treats that as one more caster with no
copy, which is the refusal the design already had.

**An empty map is not refused.** With `terrainshadow` at its default 0 and no unit caster in view
the GL twin draws nothing into its depth texture either, and the clear at 1.0 **is** the map —
every receiver then finds no blocker and is lit. Refusing that would stand the consumers down over
a map this lane reproduces with a render pass and no draw call, and it is the Classic++ default
configuration. Measured at the same 1 px as the `shadows=0` control, i.e. exactly.

#### The consumer's compare sampler had to become the twin's

`tagpu_vk_terr.c`'s compare sampler existed only so that `uShadowCmp` was a valid descriptor
(§2.30) and was NEAREST, "which keeps it off `SAMPLED_IMAGE_FILTER_LINEAR` for a depth format".
Now that it is sampled for real it has to be **LINEAR**, because the GL twin's is
(`GL_COMPARE_REF_TO_TEXTURE` + `GL_LEQUAL` + MIN/MAG LINEAR) and the bilinear filtering is half of
what makes the penumbra smooth.

Linear filtering of a depth format is a **feature bit**, so it is asked of both formats the sampler
is ever used against — the map's and the 1×1 dummy's — and a device that will not filter either
keeps the NEAREST sampler and stands the pass down on a frame that would actually sample it. The
map's format is **exposed by the shadow pass** (`tagpu_vk_shadow_format`) rather than re-derived in
the consumer, so the two files agree by construction instead of by both happening to try the same
candidates in the same order. On the reference setup it is
`VK_FORMAT_X8_D24_UNORM_PACK32` — `GL_DEPTH_COMPONENT24` exactly — and linear filtering is
available.

**So the terrain pass has a third way to stand down**, beside the restored atlas and a map this
frame's shadow pass could not draw: a device that will not filter a depth format linearly. **And
that gate is deliberately stricter than the specification**, which the review raised and which is
written down rather than acted on: VUID-vkCmdDraw-magFilter-04553 conditions the `FILTER_LINEAR`
format feature on `compareEnable == VK_FALSE`, so a depth-*compare* sampler is arguably entitled to
LINEAR without it. The reference device offers the bit either way, so nothing is lost here; being
wrong in the other direction is undefined behaviour rather than a stand-down, and this lane still
has no validation layer to settle it with. Revisit it with one running.

#### The lambert is 1 px, and it is not this landing's

The 1–2 differing pixels in the table above are the **Classic++ lambert**, and the controls say so
outright: `light=0` reads 0 px with the map on, `light=1 shadows=0` reads the same 1 px at the same
pixel with no map at all, and the heavily shadowed frame — which moves that pixel's value off
whatever boundary it was sitting on — reads 0.

It is a **newly measured** property rather than a new one. §2.30 measured the terrain pass at 0 px
on `feat-forest`, which is not a Classic++ frame; with Classic++ on the pass refused every frame
(the restored atlas), so the lambert path had never been through an oracle. `assets=0` is what
opens it, and the answer is **one pixel of 786 432, one level**, deterministic for a given camera.
Both lanes run the same GLSL — one through the NVIDIA GLSL compiler, one through glslang's SPIR-V
and the NVIDIA SPIR-V path — and a single fragment landing on a quantisation boundary is the
expected shape of the difference. **Stated as a bar, like the line tie-break**, and it is the
terrain pass's rather than the shadow pass's.

#### The fixture, and why finding one took longer than writing the pass

`terrainshadow=1` "self-shadows the ground", which is why it defaults to 0 — and at the **default**
sun (`shadowsun=225,40`) that self-shadowing is nearly invisible. Measured on `static-terrain`
(Town & Country), nine camera positions across the map, shadows on against shadows off: **0 px at
seven of them, 38 px and 54 px at the other two**. A 0-px A/B taken there proves nothing about the
map, and the first one taken here did not.

What opens it is the sun's **elevation**, which is a live knob:

| `shadowsun=225,EL` | pixels the map changes, of 786 432 |
|---|---|
| 40 (the default) | ~0–54 |
| 25 | 645 |
| 15 | 8 042 |
| **8** | **517 270** |
| 4 | 535 034 |

So `shadowsun=225,8` is the fixture: two thirds of the frame is terrain shadow and every one of
those pixels depends on the map's contents. **Confirm the picture actually depends on the thing
under test before believing a 0** — the GL-on against GL-off diff is one command and it is what
turned a vacuous pass into a measurement.

#### Not covered

* **Every caster but the heightfield** — the units, the posed bodies and the replacement meshes,
  which is the unit pass's landing. On any fixture with a unit on screen this pass refuses, and
  the terrain pass refuses with it.
* **`ss` 2**, as for every world pass (the two captures would be different sizes).
* **Classic++ `assets=1`**, because the restored tile atlas still has no CPU mirror — the terrain
  pass refuses it and the restorer's five shaders are still not in the SPIR-V pipeline.
* **The owed-teardown path**, which has still never fired on any pass: it needs the device to
  refuse a slot its resources. The lane's oldest open hole, argued and reviewed rather than run.
* **The resolution change**, i.e. the per-slot rebuild when the zoom octave moves `res`. It needs
  `zoom.on` armed and a zoom-out past the octave boundary, and this landing's fixture is at 1×.
* ~~The caster-mesh retire~~ — **covered**, see the table: `terrainshadow` is a live knob, so the
  retire fires and completes without a map change. §2.30's shared-image retire is still the one
  nothing has watched execute.
* The validation layer, still, and no device but the 4070.

### 2.33 The unit pass, drawn by Vulkan (`tagpu_vk_unit.c`, `tagpu_posebake.c`'s two mirrors) — Phase G, G19e

The **sixth world pass and the last of G19e**, and the first that **both feeds and samples another
pass's target**: it puts the posed casters into the cast-shadow depth map §2.32 draws and then
samples the finished map in its own fragment shader. That is why it has four hooks where every
pass before it had two, and why three of them are called at three different points in the seam's
frame.

| hook | where | what |
|---|---|---|
| `upload` | before `tagpu_vk_shadow_prepare` | the vertex buffers this frame's types need, the pose blocks, the texels |
| `cast` | **inside the shadow pass's render pass** | the posed casters into the map |
| `prepare` | after the map is drawn | point this slot's set at the map and the overlay; decide whether to draw |
| `record` | inside the seam's render pass | the bodies |

The GL twin has exactly this shape for exactly this reason: `tagpu_posedraw_depth_unit` runs at
`tagpu_native.c:3936` and `tagpu_posedraw_unit` at `:4238`.

**No engine address, and no new one was read to build it** — everything arrives through
`tagpu_posedraw_handover`, so there is nothing for
[exe reverse engineering](exe-reverse-engineering.html) in this landing and nothing for the hook
map above. `tagpu_vk_unit.c` is not on `thread-split.allow` and may never need to be; the list is
**unchanged** at 34 entries.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1`, **on the binary
the two findings below produced** — every figure was taken again after them and every one
reproduced.

| | |
|---|---|
| `selbox-facings`, 4 units, **2125** ink a side, Classic++ `assets=0 shadows=0` | **0 of 786 432** |
| the same with `shadows=1` — the depth twin drawn, the map sampled | **1 of 786 432**, one channel, the same pixel every run |
| `crowd-static`, **240** posed units / 32 288 triangles, sim PAUSED, **208 699** ink a side | **64 of 786 432** |
| the same at 1920×1080, **231 426** ink | **90 of 2 073 600** |
| the same at three camera stops | 51 / 59 / 65 px — a stable **0.03 % of unit ink** |
| `shadow-struct` under **Classic** (the switch off), 5 units, **9209** ink | **20 of 786 432** |
| the caster census | 256 casters in the GL map, **240 carried** → refused, the terrain pass stands down with it, both recover |
| constraint 4 | **0 of 630 784** against `main` (`2e552d1`) with `tagpu_vk.off`, on **six** pairings, over a cross-launch floor of **0** measured on each binary twice |

#### A set may only be written in its own slot's `prepare`, so the caster draw binds a second one

A descriptor set is written during its own slot's `prepare` because that is the one instant the
seam's fence proves it is not in flight. `cast` runs *before* that, inside the shadow pass's render
pass — and at that moment this slot's main set still names the map's image from the previous frame
(the same image: the views are per slot), while that image is the render target being written. So
`cast` binds a **second, smaller set** holding the two vertex-stage buffers and **no image at
all**, and the question does not arise. Two layouts, two sets a slot, one pipeline each.

#### The caster census is arithmetic now, not a flat refusal

§2.32 refused any frame whose `otherCasters` was non-zero, which is every frame with a unit on
screen. The shadow pass now subtracts what this pass is **ready** to draw and refuses on what is
left — the replacement meshes and the native 3DO stream, both still to port.

**The two counts are comparable by construction and ours can only be smaller.** `tagpu_native.c`
counts every posed unit with `castSkip` clear; this pass counts the subset of those that reached
the hand-over *and* still have a bake mirror. So `otherCasters − casters()` is 0 exactly when every
caster in the GL map has a copy here, and positive otherwise — never negative. An over-count
refuses a frame the lane could have drawn; an under-count would draw a different map, which is the
direction that must not happen. A **short** `cast` leaves the map unready rather than partial: the
draws already recorded stay (they are in a submitted command buffer either way) but `s_liveHave` is
not set, so every consumer stands down and the picture nobody draws is the one that would have been
wrong.

#### Per-unit uniforms, which is what a unit pass is

The GL twin re-uploads one 14 336-byte pose block and a dozen loose uniforms **per unit** and draws
between the uploads. A Vulkan command buffer cannot: every draw it records is submitted together.
So each unit gets its own window in three **dynamic** uniform buffers and the draw binds the set
with three offsets.

* **The pose buffer** is `TAGPU_PD_BLOCK` (14 336) a unit **per frame slot**. That size is the
  256-piece ceiling and not the model — stock's worst is 36 pieces — but a descriptor must cover
  the block the shader declares, so the window cannot be shortened to the model. 240 units is
  3.4 MB a slot. The HAND-OVER does not pay that: its rows live in an arena and 240 stock units
  cost about half a megabyte there.
* **The small buffer** carries two vertex-stage blocks (the body's and the caster's) and one
  fragment-stage block a unit, each at a device-accepted offset. The reference device's
  `minUniformBufferOffsetAlignment` is **64**, which makes that **704 bytes a unit**; the pass logs
  the number rather than assuming it (`vk: unit: up - 4 frame slots, uniform offset alignment 64,
  704 bytes of blocks and 14336 of pose per unit, compare sampler LINEAR`).

Both are grown to the frame's own unit count rather than to `TAGPU_PD_MAXHAND` (512), and both are
given back the moment the pass will not draw — the frame that hands nothing over, and **every
refusal taken before the slot is built** (the restored atlas, a build ghost, a missing mirror, the
fog grid, the overlay, a device that will not filter the map). All of those are reached before a
byte is recorded into the command buffer, so freeing the slot there rests on the same fence the
hand-over-failed path rests on. At this size §2.28's "nothing is kept once there is nothing to
draw" is the rule that makes the pass affordable at all in a 32-bit address space, and **until the
landing review it held only for the frame that handed nothing over**.

**The leak needed a session that DREW first and then met a sticky refusal**, not one armed the
wrong way from the start: every refusal above sits above `slot_sized`, so a session launched with
the overlay on never allocated the buffers to keep. The case that fits is the restorer arming
mid-session, a build ghost appearing, or a bake eviction taking a mirror away — the pass had drawn
240 units, holds 3.4 MB a slot for them (7.3 MB at the hand-over's cap), and then stops drawing
without giving any of it back.

#### A serial, not a pointer, and not a cache slot

The GL twin draws every unit out of its **type's** two static buffers, so this pass keeps one
Vulkan buffer per baked stream and uploads it once. The table is keyed on a **monotonic serial**
`tagpu_posebake.c` now stamps on every bake, because the bake's cache slots **are** reused —
`geom_slot` evicts the least recently asked-for entry and bakes another type into it, so a table
keyed on the slot, or on the entry pointer, would hand the new model the old model's vertices. The
mirror accessor bounds the pointer against its own array, checks it lands **on** an entry rather
than inside one, and only then compares the serial; a pointer into a recycled slot answers NULL and
the unit is simply not drawn, which the caster census turns into a refusal of the whole map.

**The latch drops every entry on the 0 → 1 transition**, and that is the point rather than a cost:
an entry baked before the Vulkan lane came up has no mirror, and nothing re-bakes an entry that is
still valid, so without it the pass would stand down for ever on whichever models happened to be on
screen first.

#### Two things the measurement found, and neither was a design choice

**THE VIEW WAS READ UNINITIALISED WHENEVER NO GATHER WAS ARMED.** `tagpu_native.c` filled its
`TAGPU_FXVIEW` inside `if (fxOn || sfxOn || featOn || terrOn || markOn)` and then handed that same
struct to `tagpu_shadow_begin`, which is gated on **none** of those five. With all five disarmed —
which is exactly the configuration a single pass is *measured* in — the shadow module read stack
garbage: `zoom` came out `0.000`, its light window ran to millions of texels, and the hand-over it
publishes carried a garbage frame stamp, so the Vulkan shadow pass silently found nothing every
frame **with no line in the log**. The view is filled whatever is armed now and only the gathers
are gated. It cannot happen under the play defaults, where `terr.on` is always on.

**THE UNIT FRAGMENT SHADER READS `gl_FragCoord`, AND THIS IS THE FIRST PORTED PASS THAT WOULD HAVE
SAMPLED IT.** `TAGPU_GLSL_SCAF_TEST` (`tagpu_glsl.h`) locates the fragment in the game frame with
`gl_FragCoord.xy / uSS`. **GL measures that from the LOWER left; Vulkan measures it from the UPPER
left**, and `OriginUpperLeft` is the only execution mode Vulkan permits — so with the negative
viewport height every ported pass takes for its geometry, the two are **exact mirrors**, and the
scaffold lookup would land on the wrong end of the overlay and cut the wrong fragments through its
`discard`. Three ways out were considered and all three are worse than standing down:

* editing the macro to take the origin as a uniform changes the GL twin, which is both the oracle
  and the shipped renderer;
* re-deriving `uScafP` so the mirrored coordinate comes out at GL's value is possible on paper
  (`w = −vh`, `y = C − vpT` with `C` folding the zoom un-transform) but it is the port re-deriving
  its own inputs, and it breaks silently the day the macro changes;
* mirroring the uploaded overlay does **not** cancel — it would need the viewport's top and bottom
  margins to be equal, and they are 32 and 33.

So a frame with the overlay armed is **refused**, said once, and the reason is written down rather
than worked around. It costs nothing in play: `scaffold.on` is not in the default arm set. **Any
later pass whose fragment shader reads `gl_FragCoord` inherits this** — the hi-res path and the
effects pass both carry the same macro.

#### What the landing review found, and four of the five were the seam rather than the shader

Two Opus reviewers at `high`, one on correctness and one on synchronisation alone. **Both led with
the same finding**, independently, and it is the one below.

**A BAKE ENTRY THIS FRAME STILL NEEDS COULD BE EVICTED TO MAKE ROOM FOR ITS OWN SIBLING.**
`vb_slot` refuses to evict an entry stamped with the frame in hand, and `upload` stamped the pair
*after* resolving both — so a unit whose geometry was cached and whose material was not handed
`vb_slot` its own geometry entry as the least recently drawn. The entry is retired and reused for
the material, `g` and `m` then name the **same** entry, and the body draw fetches 32-byte vertices
out of a buffer holding 20-byte ones: past its end, on a unit that still passes the
every-unit-or-none gate because it *was* drawn. The stamps moved to the instant each entry
resolves. The same eviction had a second mouth: `stageNeed` is counted before a byte is uploaded,
so an eviction inside the loop turns a later unit's cache hit into a miss whose bytes were never
reserved, and the staging `memcpy` runs past the mapped allocation. That one is now **bounded
against `s->vscap`** rather than against the pre-pass, because a pre-pass cannot budget for the
eviction it is trying to budget for; a unit that will not fit is not drawn, and the gate below
turns that into a refused frame — and the bound carries the **atlas's reserved share** of that same
allocation with it, because `atlas_upload` copies into the tail of it after the loop and an
overspend small enough to leave every unit fitting would have run the atlas memcpy past the end
instead. Reachable once the 512-entry table fills, which two map loads do.

**THE COMPARE-SAMPLER REFUSAL WAS TAKEN ONE HOOK TOO LATE.** It lived in `prepare` — which runs
*after* `cast` has put this frame's casters into the map and after the shadow pass has published
it. So on a device that will not filter a depth format linearly the bodies stood down while the
terrain pass went on sampling a map those bodies are in: unit shadows lying on terrain with no
units above them, every Classic++ shadow frame, silently. It is decided in `upload` now, where
`tagpu_vk_unit_casters` still answers 0 and the census refuses the map with it. **The hook that
owns a decision is the earliest one that can take it**, not the one that needs the answer.

**THE WORLD SCISSOR WAS NOT SCALED.** The rect arrives in game-frame pixels and the viewport covers
the whole attachment, exactly as in §2.29–§2.31 — and `unit_scissor` was `feat_scissor` without its
`w/gw`, `h/gh`. At the 1:1 sizes an A/B is run at the two are the same number, which is why 786 432
pixels of measurement could not see it; a 640×480 game frame in a 1920×1080 window would have
clipped every unit to the left third of the bottom quarter.

**A BARRIER SAT BELOW A GATE THAT CAN RETURN.** The `TRANSFER_WRITE → VERTEX_ATTRIBUTE_READ`
barrier was recorded after the every-unit-or-none gate. That gate returns without drawing, but the
copies are already in the command buffer and the buffers keep their serials — so a later frame
finds them, uploads nothing, and reads vertices no barrier ever ordered against the write that
filled them. Recorded before the gate now.

The fifth is the give-back above. **Nothing was rejected**; the two minor notes (a latch shared by
two different messages, and a comment claiming the fog cell count is "not always `cols*rows`" when
every assignment makes it exactly that) were both corrected.

#### The scaffold question: half answered, and the half that is says which half is not

The handoff asked this landing to decide, once, how a pass samples another pass's texture — for the
units, the hi-res path and the effects. **The mechanism is settled and is in place**:
`tagpu_vk_scaffold.c` exposes a **frame-stamped per-slot `VkImageView`** in exactly the shape
`tagpu_vk_shadow.h` arrived at, its `prepare` moves to the top of the seam's frame while its
`record` stays over the world, and this pass points binding 44 at the real image. **Prepare order
is not record order**, which is new to the seam and stated at both call sites.

What is *not* settled is the paragraph above. Binding 44 names the overlay anyway, so it is the
descriptor that will still be right when the fragment-coordinate half is answered.

#### The residual, and it is measured rather than asserted

The 64 px are a **stated bar**, of the same family as the effects pass's line tie-break (§2.31) and
the terrain pass's one-pixel lambert (§2.30, §2.32) — but larger, because a unit frame is 32 288
triangles of dense sprite art rather than a terrain grid. What was established about it:

* **Deterministic.** On a frozen sim each lane is **byte-identical to itself** across two captures,
  and the differing **set** is the same both times. So it is not a race, not an ordering, and not a
  mirror that had not converged.
* **Per pixel, not per face.** **48 of 55** 8-connected clusters are **one pixel** and the largest
  is **three**; there are **58 distinct (GL, Vulkan) colour pairs across 64 pixels**. A flipped SHD row would
  repaint a whole triangle uniformly and give a handful of pairs, so the shade quantisation is not
  what this is.
* **Mostly colour, a little coverage.** **5 of 64** have one lane black; the other 59 are colour
  changes at pixels both lanes covered.
* **Not the lighting and not the map.** It is there with the lambert on and off, with the shadow
  map on and off, and under **Classic** with the whole Classic++ switch removed.
* **A texel boundary, looked at.** Zooming one of the runs shows a **horizontal texture-row
  boundary picked one row apart** along eight pixels — the GL lane takes the dark row under a light
  band, the Vulkan lane continues the light one.

That last one is **§3.0's own finding in a second lane**: a fragment centre landing exactly on a
texel boundary, which the terrain pass fixed with a 1-texel replicated border **plus**
`TAGPU_EDGE_NUDGE`, a 1/32 game-pixel offset in its vertex shader. **The unit path has no nudge**,
and the 16.16 snap puts posed vertices on an exact grid over art whose UVs are exact texel
multiples, so a great many fragment centres land exactly on a boundary. In one lane the tie goes
one way and in the other it goes the other.

**The candidate fix is therefore the terrain's, and it is not this landing's to take.** Adding the
nudge to the unit vertex shader would change the **shipped GL renderer** to serve the port, which
is the owner's decision and not a session's; the terrain's own note records that its 1× output came
out bit-identical, which is evidence the cost may be nil, not proof of it for units.

**What is NOT established** is that the last-bit difference is the two compilers rather than
something in the port. The lambert, the shadow map, the restored atlas, draw ordering and every
per-face term are eliminated; the interpolated value itself cannot be read from here, and this lane
still has no validation layer.

#### Not covered

* **The nanoframe WIRE, the Classic SILHOUETTE and the structure SLANT** — the same program and the
  same bake at `uRange` 2 and 1. They draw *outside* the window the A/B brackets, so they cannot
  make this comparison disagree, and the pass does not refuse for them; they are simply missing
  from a Vulkan frame that has them in the GL one. The wire also draws **lines**, which is §2.31's
  stated bar.
* **The replacement meshes** (`tagpu_hires_draw.c`) **and the native 3DO stream's own unit
  vertices** — the effects models and the selection lines. Both still count into the shadow map's
  census, which is what refuses a frame with either in it: measured at 256 casters against 240
  carried on `crowd-static`, whose 16 ARMPWs take the hi-res path.
* **The BUILD GHOST**, which rides the same entry points in a second window: the hand-over counts
  it in `otherDraws` and the pass stands down.
* **Classic++ `assets=1`.** The refusal is written and is the same shape as the feature and terrain
  passes', but it was **not exercised**: the restorer never armed the unit twin in this session, so
  `uRestored` stayed 0 on both lanes and the frame was drawn rather than refused.
* **`ss` 2**, as for every world pass (the two captures would be different sizes).
* **The owed-teardown path**, which has still never fired on any pass, and the per-type buffer
  retire, which needs an eviction the 512-entry table did not reach.
* The validation layer, still, and no device but the 4070.

### 2.34 The UI layer's 1x mirror, drawn by Vulkan (`tagpu_vk_gui.c`, `tagpu_gui_surf.c`'s op mirror) — Phase G, G19f landings 1–5

**The first pass of G19f, and the first ported thing that is not a draw over a mesh.** §2.3e's UI
layer is a **stateful store of per-surface twins** that an op stream mutates, plus a three-layer
composite. The state is the whole difficulty and it is what decided the cut:
[the G19f plan](g19f-plan.html) has the landings.

**Nine shaders, none of them new.** G19c already translated `QVS`, `CPY_FS`, `SPR_FS`, `STR_FS`,
`CURS_FS`, `MM_FS`, `SHARP_FS`, `LAY_VS` and `LAY_FS`. Landings 1 and 2 use **five** of them —
`QVS`, `CPY_FS`, `SPR_FS`, `STR_FS` and the `LAY_VS`/`LAY_FS` composite — and write no GLSL.
`CURS_FS`, `MM_FS` and `SHARP_FS` are the sharp layer, which is landing 3.

**Landing 1 was the mirror with `PK_STRING` refused; landing 2 is the string op.** They are one
section because they are one pass and the second is the first's machinery with one more program.
The distinction that matters to a reader is that **landing 1 alone composited nothing in real
play** — text is on screen in essentially every in-game frame, so the capability refusal below
fired on all of them and only `gui.on=nostring` produced a picture to compare. Landing 2 is what
made the pass a thing that draws the game's UI rather than a thing that draws a fixture's.

#### The op stream is dead by the time the Vulkan lane runs

`render_ogl.c`'s iteration is `tagpu_packet_acquire` → `tagpu_overlay_draw` (inside which
`tagpu_gui_present` drains) → `tagpu_packet_frame_end` → `tagpu_vk_frame`. `drain()` advances the
queue's arena tail **per op**, so the game thread may overwrite the bytes those ops point into the
instant it returns — two calls before the Vulkan lane exists. A pass reading `g_guiq.arena + aoff`
would be §2.30's fog-grid bug on a 16 MB buffer.

So `tagpu_gui_surf.c` grows an **opt-in mirror**, filled inside `drain()` as each op is applied,
carrying a copy of the op and of its payload. The alternative — deferring the tail publication
until after `tagpu_vk_frame` — was rejected because the release would have to run on every path
through `tagpu_overlay_draw` including its three early returns, and it would make the GL queue's
lifetime depend on a lane that is normally not armed. **Nothing is copied until
`tagpu_gui_mirror_want(1)`**, which is the only reason copying an op stream that can run to
thousands of ops a present is affordable at all.

**It is recorded where each op is APPLIED, not where it is read.** The hand-over says what the
render half DID: a sprite whose atlas entry would not resolve drew nothing, and a copy whose
source has no twin is a reseed rather than a draw. And it is a **public struct of its own**
(`TAGPU_GUIOP`), not the producer's private queue op, so a change to the queue cannot silently
change the port's contract.

Three things ride along that the port must not re-derive: **the atlas rect the GL lane resolved**
for each sprite (a second lookup could answer differently after a repack, and the A/B would then
be comparing two atlases), the UI atlas's texels through `tagpu_gaf.c`'s existing CPU mirror, and
**the engine's own frame** — the composite's bottom layer and its stale-mirror guard, which the GL
lane has as `f->surface_tex`, a GL texture, and which is therefore copied out of the fork's primary
under `g_ddraw.cs` on the same lifetime argument §2.3f makes for the palette.

#### The flip is a property of presentation, and this is its clearest case

§2.32's rule decides every coordinate question here. The **twin draws take no flip**: their target
is SAMPLED, and `QVS`'s `y / uSize.y * 2 - 1` puts quad y = 0 at attachment row 0 under both APIs.
`CPY_FS` reads `gl_FragCoord` — the macro §2.33 had to stand the unit pass down for — and with no
flip GL measures it from NDC −1 while Vulkan measures it from attachment row 0, **which is the
same row index**. So it ports unchanged and §2.33's refusal does not arise. The **composite does
flip**, because the composite is presented.

That was derived against `twin_copy` and `QVS` rather than reasoned from conventions, and the
first draft of the plan got it wrong in the other direction — it claimed the GL lane already
contained a compensating mirror. It does not. There is no flip anywhere in `tagpu_gui_surf.c` and
the module's own comment says so.

#### The twin store is the first shared mutable state on this lane

Every world pass through G19e keeps its state **per slot**, so the seam's `fence[slot]` wait is the
whole argument. A twin cannot be per slot: it is persistent state an op stream mutates across
frames, and 32 of them per slot is the memory this phase spends its budget measuring. So they are
shared — and frame N writes them while frame N−1 may still be sampling them in its composite, a
write-after-read across submissions.

**The DATA hazard is closed by an ordering.** A `vkCmdPipelineBarrier`'s first synchronisation
scope includes every command submitted previously to the same queue, so one barrier at the top of
the replay — `FRAGMENT_SHADER`/`SHADER_READ` before
`COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE` — orders this frame's twin writes after every
earlier frame's composite reads. Both landing reviewers checked that argument and neither could
break it. **There is still no validation layer in the wine prefixes**, so it is read against the
spec and not checked by one.

**AND IT SAYS NOTHING ABOUT LIFETIME, WHICH IS THE MISTAKE THIS SECTION ORIGINALLY MADE.** Ordering
a write after a read does not keep the object alive to be read. The first version of this pass
destroyed a twin's image, view, memory and framebuffer inside `prepare` — while `slots − 1` earlier
submissions were still executing and while the command buffer being recorded already named them —
and did the same to the shared atlas and engine images on a dimension change. A `PK_FREE` for the
surface the previous frame presented is a use-after-free on the **first** eviction. **Both
reviewers led with it, independently**, and the file already contained the correct rule: the
`refuse` label destroys nothing and says why.

The answer is the lane's own **slot-bitmask retire**, the one §2.30's terrain pass and §2.33's unit
pass already use — every slot's bit set at push including the one being recorded, cleared when that
slot comes round again behind its fence, destroyed at zero, and a full list stops the pass rather
than destroying anything. **No Vulkan OBJECT is destroyed from `prepare` any more** — the two
`kill_buffer`/`slot_free` sites that remain are the slot's own host-visible buffers, which are
safe under the fence the seam already waited on and for which every world pass makes the same
argument. That sentence read "no destruction at all" until the re-review pointed out it was false
and, worse, told the next reader not to look — which is exactly how the original bug got in.

#### The replay runs on frames the composite cannot

**This is the landing's own hardest-won line, and the first version of it was half a fix.** The
refusals — a Classic++ colour twin, coverage in the sharp layer, a missing engine frame — first
returned before the replay. But the GL lane applies those ops whatever it draws, so a frame the
port skips leaves its twins behind the GL lane's **for the rest of the session**, silently. Those
three gate the composite now and the replay runs regardless.

**What the review found is that five other paths had the same shape and were missed**, and that a
sixth was on the producer's side:

* `standdown` dropped the store and asked for nothing, so the next frame found no presented twin,
  landed there again, and **the pass never drew again for the session with no line in the log**.
* Pass 1's eleven bare `return 0`s did it for one malformed op.
* An abandoned mirror frame (the op array or arena refusing to grow) did it silently, counted by a
  statistic nobody read.
* **`PK_STRING` is the one that matters most**, because it is not a bug in a rare path: the GL lane
  *applies* it (`twin_string`) and landing 1's hand-over did not carry it, by design. Treating it as
  a `compose = 0` meant those glyphs were in the GL twin and would never be in ours. **The sentence
  that stood here — "the twins are kept level" — was written of the ops the mirror carries and was
  false of the one it does not.** Landing 2 carries it, so this is now a statement about the shape
  of the refusal rather than about strings: `otherOps` is **0 for every op kind that exists today**
  and the machinery stays, so the next kind added to the queue lands there rather than being drawn
  wrong. A capability gap asks for **nothing** — a fresh start cannot help when the very next frame
  carries another one — and the store is caught up with one fresh start when they stop.
* And `mir_finish` published nothing when `draw_layer` returned early, on frames whose ops the drain
  had already applied — the same hole the consumer had just been restructured to close, still open
  at the other end. It publishes with `presented = 0` now: replay, do not composite.

So **falling behind is a STATE**, with one way in and one way out: drop the store, raise the
producer's own `reseed`, composite nothing, and apply nothing but the RESET that answers it. That
is `tagpu_gui_surf.c`'s own `s_skipToReset`, for the same reason. **Measured firing and recovering**
on the binary the fixes produced.

**"ONCE, ON THE TRANSITION" IS ONCE PER FRAME WHEN THE TRANSITION REPEATS, AND THAT IS THE RESEED
STORM AGAIN.** The re-review of landing 1 caught `behind()` raising the producer's flag every frame
and made it act on the transition only. Landing 2's review found the same storm through the other
door: a RESET **clears** `s_behind`, and a condition that is structural rather than transient fires
again inside the very frame that answered it — so the pass asks, is answered, asks again, at the
frame rate, and the thing being reseeded is **the GL lane's own twin store**. The asks are capped
now, and the cap counts **fruitless** asks: a run of frames that got through **the replay** is the
evidence the last fresh start worked and gives the budget back, so a map change still recovers while
a structural condition goes quiet after eight tries and never touches the producer again. A
capability gap costs the oracle nothing; that is the point of telling it apart from a sync gap.

**"A run of frames that COMPOSITED" is what that said first, and the re-review disproved it out of
this very page.** `compose` is 0 whenever the sharp layer has coverage — *"every frame with a cursor
on screen"*, as "Not covered" below already stated — so the refund sat on a branch an ordinary
session never reaches, every legitimate transition counted against the cap, and four level loads
muted the pass for good. Being **level** is a property of the replay, not of the composite. The
evidence against the claim was two screens down in the same section.

**AND A LOST FRAME NOW RE-ASKS.** `g_guiq.reseed` is **one-shot**: the producer clears it the
instant it publishes the RESET. If the present carrying that RESET is itself abandoned — and it is
the likeliest one to be, being every surface seeded at once and so the largest arena there is — the
answer never arrives, `s_behind` is already 1 so nothing asks again, and the replay skips every op
for the rest of the session waiting for a RESET that will never be sent. Publishing the lost frame
(above) turned *silently* behind into *permanently* behind with one line in the log. A lost frame
therefore invalidates an outstanding request. That is not "the window is small": it is a message
provably not delivered, re-sent.

**Three `continue`s were doing the same thing quietly, since landing 1.** An op naming a surface
this store never seeded, and a pixel op outside its own twin, were skipped — the GL lane applies
them and we never do. The composite's own `tw_find(h.presented)` catches that for the **presented**
surface only, and only on a frame it draws. They are the behind state now, and the fixture shows
they were firing the whole time: the two events a shell→game transition produces used to report
*"the presented surface has no twin here"* and now name their actual cause.

#### The string op is the twin store's, not the sharp layer's (landing 2)

**`twin_string` stamps TA's own glyphs into the TWIN** (G17d), from a per-font glyph cache the
render thread rasterises. §2.3e's "the sharp layer is empty until the cursor and the string op fill
it" is about the *sharp* text path; this one is a draw into the same `RG8` twin every other op
writes. So the port is landing 1's store plus one pipeline — `STR_FS` on the same descriptor layout
as `SPR_FS` and `CPY_FS` — and not the device-resolution layer the plan had bundled it with.

**The texels needed no mechanism.** `tagpu_text.c` already keeps its glyph atlas as a CPU array
(`s_gatlas`) and uploads the GL texture *from* it. `tagpu_text_glyph_atlas()` hands back that array
and its dimensions — one accessor where the feature pass needed a whole opt-in mirror for the same
question.

**BUT IT NEEDED A SERIAL, AND THE ONE THAT WAS ALREADY THERE WAS THE WRONG ONE.** The first draft
keyed the `R8_UNORM` upload on `tagpu_text_glyph_gen()`. **That counts REPACKS** — the two paths
that throw the whole atlas away — and says nothing about an ordinary glyph being rasterised into a
fresh shelf, which changes the atlas's bytes and leaves every previously issued cell valid. The GL
lane never had to tell the two apart because it re-uploads on `s_gdirty`, a plain dirty bit. So the
Vulkan image was uploaded **once** and every `(font, code)` pair first seen afterwards — a new digit
in a resource counter, a unit name not yet displayed, a second font — stayed **0** in it: the glyph
invisible where `bg == tr` and a solid `bg` box where it is not, permanently, for the session, with
no counter anywhere saying so. `tagpu_text.c` now publishes a **content serial** beside the
generation, bumped wherever `s_gatlas`'s bytes change and nowhere else (not when the GL *texture* is
re-created, which is liveness). **Both landing-2 reviewers led with this, independently** — the
sixth such pair on this lane, and the measurement could not have found it: the fixture's whole glyph
set was in the atlas before the first upload, which is exactly the shape that reads 0 px.

**AND A REPACK MID-PRESENT INVALIDATES EVERY STRING ALREADY RECORDED IN IT.** `twin_string`'s retry
is correct *for itself*: it drew into the GL twin from the texture as it stood at the generation it
resolved against. The mirror does not draw until the drain is over, against the atlas as it stands
**then** — so a repack caused by a *later* string in the same present leaves the earlier strings'
cells naming cleared texels. The producer records the generation with the first string of a frame
and **loses the frame** if it has moved by `mir_finish`. Which introduces the third fix:

**AN ABANDONED MIRROR FRAME USED TO BE WITHHELD, AND A WITHHELD RECORD IS INDISTINGUISHABLE FROM AN
UNARMED LANE.** The consumer's "nothing handed over" branch reads it as *the GL lane drew nothing
either* — true when the lane is not armed, false when the op array or the arena refused to grow
mid-frame, and in that case the store is a frame of ops behind, silently, for the session. The
record is published now with a `lost` flag and the consumer answers it with the behind state. This
is the same hole the landing-1 review closed at the `!s_mLayer` end of `mir_finish` and left open at
this one; it was found verifying the repack fix, and **neither reviewer named it**.

**THE CELLS ARE CARRIED, NOT LOOKED UP, and for a harder reason than the sprite's.** The sprite's
atlas rect is carried because a repack between the two lanes' lookups would answer differently.
A string is worse: `twin_string` resolves its glyphs against an atlas that can **repack in the
middle of one string** — it takes the generation before and after and retries once for exactly
that — so a second lookup here could name texels that moved while the GL lane was drawing, and the
A/B would be comparing two atlases rather than two rasterisers. The hand-over carries the four
`short`s a glyph that the GL lane settled on, `nglyph` of them in the mirror arena, plus:

* **the pen it started from**, which is `o->sl` and `o->st − yoff`. `0x4CCF60` writes its first
  pixel row at `y − (s8)font[2]`, not at the `y` it was given (§"The in-game bitmap font" in the
  engine map), so the *resolved* top is carried rather than the op's.
* **the three colour bytes** `fg`, `bg` and `tr` — the blitter's own three arguments, which
  `STR_FS` takes as `uFg`, `uBg`, `uTr`.

`mir_string` is called from the **tail** of `twin_string`, after the draw succeeded. A string that
stamped nothing takes the `reseed:` label instead and raises the producer's own `g_guiq.reseed`, so
both lanes are level: neither drew, and the RESET that answers it reaches the mirror as an op like
any other. An arena that will not grow abandons the mirror frame, which is `behind()`'s business.

**A STRING SETS NO SCISSOR**, because `twin_string` calls `glDisable(GL_SCISSOR_TEST)` outright
where `twin_sprite` and `twin_copy` enable it. The port sets a full-twin scissor rather than the
op's box; clipping to the box would cut glyphs the GL twin has.

**One block, `nglyph` quads.** Every glyph of a string shares its three colours and the twin's
size, so the descriptor set is bound once and the vertex offset walks — which is what the GL lane
does with one program and n draws. That made the vertex count stop being the draw count, so the
**vertex slots and the uniform windows are counted separately**: `nquad` sizes the vertex buffer
(and positions the composite's own quad at the end of it), `ndraw` sizes the uniform and flags
buffers. They were one number through landing 1 because every draw was exactly one quad; keeping
them one here would have put the composite's own quad, which sits at the end of the vertex buffer,
on top of a glyph's.

#### The sharp layer, and why it needed none of the twins' machinery (landing 3)

The third client of the composite is a single `RGBA8` at **device** resolution, row 0 the
viewport's top, that the GL lane clears to `(0,0,0,0)` at every present and the composite takes
wherever its **alpha** says it has coverage. Landing 1 wired `uSharpOn` and left it 0; landing 3
turns it on.

**IT IS PER SLOT, AND THAT IS ITS WHOLE SYNCHRONISATION ARGUMENT.** A twin had to be shared
because it *accumulates* — which is what forced the barrier reasoning above, the slot-bitmask
retire, and the use-after-free that got through the first time. This layer keeps nothing across
frames, so it is per-slot state that the seam's own `fence[slot]` wait already covers, and none of
that reasoning applies to it. Its render pass is the twins' opposite — `CLEAR` where they `LOAD`,
for the opposite reason — and the clear is *transparent* rather than merely black, because alpha is
what the composite gates on. It takes **no flip**, because like a twin it is sampled rather than
presented (§2.32).

**What crosses is a short ORDERED LIST of quads, not texels and not an op stream**, because that is
what the layer is: three clients drawn in a fixed order — the harness's flat quads, the cursor, then
the minimap's base and its view box. At most 16, carried **by value**, which retires any question
of what they point into.

**THE CURSOR'S RECT IS WHY THE LIST CARRIES RESOLVED GEOMETRY.** `sharp_cursor` takes the pointer
position from `mouse_last_client()` **at draw time**, and the Vulkan lane runs later in the same
iteration of `render_ogl.c`'s loop — so re-deriving it there places the cursor where the mouse has
moved to *since*. That is a guaranteed non-zero A/B which would have read as a rasteriser
difference. The resolved destination rect crosses instead, with the atlas rect and the colour key.
**Same rule as the sprite's atlas rect and the string's glyph cells, and the third landing on this
pass it has decided.** The minimap's box (the packet's, after the HUD map and the `hq8` scale), its
view rect (`main+0x142CB`, four edges at one *game* pixel each so the box keeps its weight as `k`
grows) and that box's colour, already through the presented palette, cross for the same reason.

**Two things reading the code corrected, each of which would have cost a measurement round:**

* **The engine's minimap pair is `RGB8`, not `RG8`.** The GL source calls it "the engine's two
  bases" and the plan repeated that, but `MM_FS` reads **three** channels — `r` the fogged base,
  `g` the unfogged one, `b` the composite the engine drew its dots, radar arcs and points into —
  and the CPU loop beside it strides by 3. Both minimap images are widened to `RGBA8` on the way
  into Vulkan, because `VK_FORMAT_R8G8B8_UNORM` is optional and rarely supported while the
  four-channel one is universal, and `MM_FS` reads `.rgb` either way.
* **`SHARP_FS` is needed by this landing**, which the plan doubted on the grounds that `LAY_FS`
  already samples the layer. It is not the sampler; it is the *flat quad*, and the minimap's view
  box is four of them.

**One descriptor layout serves all three programs**, because G19c's translation had already
normalised them: a 16-byte block at binding 32 each, and samplers at 40..42 for `CURS_FS`
(`uAtlas`, `uAtlasRGB`, `uPal`) and `MM_FS` (`uPic`, `uEng`, `uPal`). So the sets are indexed by
**kind** rather than by draw — a frame's up-to-16 quads sample at most three distinct combinations
of images — which is three sets a slot instead of sixteen.

**What the landing review found, and the one that could not be measured.** Eight findings across
two reviewers, and **both led with the sampler independently** — the seventh such pair on this lane:

* **THE MINIMAP PICTURE IS MINIFIED `LINEAR` IN GL AND WAS POINT-SAMPLED HERE.** This module had one
  sampler and a comment saying every texture it reads is nearest. That was true until this landing:
  `MM_FS`'s own comment explains why the picture is the exception — the destination box is smaller
  than the 252-px picture at every `k` it draws at, so every fragment takes the *minification*
  filter, and a downsample wants one. GL blends four texels there; a nearest sampler takes one. A
  second sampler is the whole fix, and the A/B could not see the bug for the reason above.
  **THE FIRST VERSION OF THAT FIX WAS A NO-OP AND THIS SECTION RECORDED IT AS CLOSED.** The new
  sampler inherited `maxLod = 0.0f` from the nearest one it was copied from; Vulkan clamps the
  level-of-detail to `[minLod, maxLod]` and only *then* decides magnification (λ ≤ 0) against
  minification, so with both 0 the answer is always magnification and `magFilter` — still nearest —
  is always what runs. `VK_FILTER_LINEAR` never executed. **An open divergence written down as
  closed is worse than one written down as open**, and `tagpu_vk_unit.c` already carried
  `maxLod = 0.25f` for exactly this reason, in this repository. [The re-review caught it.]
* **THE ENGINE'S MINIMAP PAIR WAS ALIASED PACKET MEMORY, NOT COPIED.** The comment claimed the
  hand-over's frame rule covered it. It does not: the packet's rule is **stricter**, and
  `tagpu_packet_frame_end` gives it back *before* `tagpu_vk_frame` runs in the same iteration — so
  the consumer read it after its owner released it. `tagpu_packet.poison` fires at that give-back,
  which means the lever built to catch exactly this kind of mistake could not see it either. It is
  copied now, as `eng`, `mmPic`, the atlas and the palette all already were. Its **dimensions come
  from the same read as its bytes**, so the two cannot disagree.
* **A refusal that still consumed its input.** The minimap guard cleared `compose` but left
  `needMM` set, so the two cases it names — a null pointer, a dimension past the cap — were fed
  straight into the staging path below, which reads `w * h * 3` bytes from the very pointer it
  refused.
* **`h.nsdraw` sized two allocations before its own validation gated anything**, which is this
  file's own stated rule ("a handed-over count never sizes an allocation") being broken.
* **`s_palView` was the one view bound with no `Have` guard**, so a null handle could reach
  `vkUpdateDescriptorSets` on the frames before the palette first resolves.
* **`s_sharpInk` was never reset** where `s_sharpOn` is reset on every call, so a session that once
  had coverage and then took an early return published `sharpOn = 1` with no quads for ever — which
  this pass reads as "the GL lane has coverage I did not produce" and stops compositing.
* Two smaller: the layer was recorded **above** the composite gate, so it was drawn and discarded on
  every frame the composite was stood down on (every frame of a Classic++ session); and the two new
  refusals said nothing in the log.

**And the re-review found two more of mine, both in the fixes above.** The refusal logs it asked for
printed **at the frame rate**, because `s_saidSharp = 0` was the first statement of the branch that
then tested `if (!s_saidSharp)` — the latch never held, so a condition holding every frame printed a
line every present. And **the budget refund counted frames on which the store is provably not
level**: while `s_behind` stands the replay applies *nothing*, so those are exactly the frames that
prove nothing, and counting them let a pass waiting for a fresh start that never comes refund its
budget for ever and go on asking the **oracle** for a reseed with the mute unable to latch. That is
the **third** version of the same mistake on this pass, and the comment beside it already said what
to check.

One was mine and not theirs: publishing the engine pair through `mir_bytes` made a **latent**
staleness reachable — `mir_finish` had already taken `arena` and `alen` further up, so a realloc
there would have left every op's `aoff` pointing into freed heap. Re-published after the copy.

**An overflowing list is a `compose = 0` and NOT the behind state**, and that distinction is the
one the twins established the hard way: a sharp-layer quad mutates nothing that outlives its frame,
so a frame the pass cannot draw is *one frame*, never a store out of step. Nothing is asked of the
producer for it.

#### A set is claimed for one image for the length of a frame

A descriptor set may not be rewritten once a recorded draw names it, and the twin **array** cannot
be the index: `tw_drop` moves the last entry into the hole, so the indices shuffle under it. The
claim is on the **view** instead, and `SET_MAX` bounds how many a frame can want. There are exactly
three kinds of image a twin draw samples — the one UI atlas (every sprite), the one glyph atlas
(every string) and a copy's **source twin** — so the bound is **the twins plus two**.

**It was the twins plus ONE until landing 2, and the string op is what made that wrong.** The old
comment's argument was "every sprite samples the one atlas", which stopped being true the moment
there were two non-twin images.

**AND `TW_MAX + 2` IS NOT A BOUND EITHER — IT IS A SIZE.** The landing review split on this and the
dissenting reviewer is right: `tw_drop` **deliberately** leaves a claim standing (clearing it was a
previous round's fix and caused a worse hazard — a set a recorded draw already names being
rewritten), so what a frame counts against `SET_MAX` is **distinct views claimed**, not twins alive.
A present that batches FREE + SEED + COPY churn claims a view per *generation* of a surface, and
nothing bounds that by `TW_MAX`. What IS by construction is the consequence: `set_claim` answers 0
deterministically, the replay goes to `standdown`, and `behind()` drops **our** store and asks the
producer once. Nothing is corrupted and nothing of the GL lane's is touched. Sized for the case
that has to work, degrading predictably past it — stated here rather than dressed up as a bound.

#### Two things the measurement found, and both were the port's

**`s_sharpOn` MEANS "THE LAYER EXISTS", NOT "ANYTHING IS IN IT"** — and it exists on every frame
once it has been made. The pass refused on it, so with `nocursor nominimap nostring` armed it stood
down on **every** frame while every counter that would explain why read zero. An empty layer and a
disabled one composite identically (it is taken only where its alpha says it has coverage), so the
port may draw a frame whose layer is empty and must refuse one whose layer is not. `s_sharpInk` is
that question, answered from the two clients' own counters.

**`DRAW_MAX` WAS SIZED FROM A FLIP AND THE HAND-OVER CARRIES A PUBLISH.** 4096 came from "a steady
screen is ~41 ops of which a handful draw", which is true of a flip; the publisher batches every
flip since its last one and the shell flips ~12 000 times a second at a 5 ms cadence. Measured at
**7414** on a 1080p level load.

#### Measured

**LANDING 1, MEASURED 2026-09-16** on the binary the LANDING REVIEW's fixes produced (every figure
re-taken after them), `gui.on=nostring nocursor nominimap norestore`, `vk.on=color=0,0,0`, Classic,
reference setup's 4070. **Each capture's own size is what confirms its fixture** — see the trap in
`ta-drive`:

| | |
|---|---|
| shell `MAINMENU`, 640×480, two runs | **0 of 307 200**, 306 737 ink a side |
| `selbox-slope` in game, 1024×768 | **0 of 786 432**, 748 890 ink a side |
| `selbox-slope` in game, 1920×1080, two runs | **0 of 2 073 600**, 1 989 791 ink a side |
| one earlier shell run's two files | **byte-identical**, which is the strongest form of a 0 |
| the behind state | **fired, asked for the fresh start, and recovered to 0 px** |
| the twin store doing the work | 2 twins; millions of sprite ops, tens of thousands of copies |

So the op stream, the seeds, the pixel ops, the sprite quads through the resolved atlas rects, the
copies through `CPY_FS`, the palette and the engine's own frame beneath all reproduce the GL twin
exactly.

**LANDING 2, MEASURED 2026-09-16 WITH STRINGS ON** — `gui.on=nocursor nominimap norestore`, and the
absence of `nostring` is the whole point of the table. Same binary discipline, same lever, same
fixture:

Re-taken on the binary the **re-review's** fixes produced, every figure:

| | |
|---|---|
| `selbox-slope` in game, 1024×768 | **0 of 786 432**, `str=12/48 miss=0 reseed=0 repack=0 glyphs=27 fonts=1`, `mirlost=0` |
| `selbox-slope` in game, 1920×1080 | **0 of 2 073 600**, `str=6/25`, `mirlost=0` |
| the capability refusal | **gone** — no stand-down line at all, where landing 1 fired one on every in-game frame |
| the behind state | fired twice, both at the shell→game transition, both recovered, no mute |
| what those two events were | `an op names a surface this store never seeded` and the pixel-op form of it — the holes the re-review found, which used to be reported as the presented surface having no twin |
| the states the fixes added | **none fired**: no lost record, no ask cap reached, no quad bound, no dimension refusal |

**LANDING 3, MEASURED 2026-09-16 WITH THE SHARP LAYER ON** — `gui.on=norestore mmbase`, and the
absence of `nocursor` and `nominimap` is the point of the table. `mmbase` forces the sharp minimap
at `k = 1`, which it would otherwise leave to the engine. Same fixture, same discipline:

| | |
|---|---|
| `selbox-slope` in game, 1024×768 | **0 of 786 432**, `sharp=1024x768` |
| `selbox-slope` in game, 1920×1080 | **0 of 2 073 600**, `sharp=1920x1080` |
| the cursor | `curs=1,21x23,dev=0,sc=1.00,drawn=20990` and `11388` — it is on screen and drawn, not absent |
| the minimap | `mm=10810` and `2559` |
| the strings, still | `str=7/30 miss=0 reseed=0 repack=0 glyphs=27 fonts=1`, `mirlost=0` |
| the sharp stand-down | **gone** — where landing 2 fired it on every frame with a cursor on screen |

**THE COUNTERS ARE THE CHECK THAT THIS 0 IS NOT VACUOUS, not the ink figure** — the lesson landing
2 wrote two screens up. A layer the Vulkan lane drew *nothing* into, while the GL lane drew a
21×23 cursor and a minimap, would differ by thousands of pixels and not by none. `drawn=` and `mm=`
are what say both lanes had something to compare.

**AND ONE PATH INSIDE THE MINIMAP IS STILL NOT COVERED BY IT, WHICH THE REVIEW FOUND AND THE
COUNTER CONFIRMS.** `MM_FS` has two outcomes: where the engine's 3×3 neighbourhood is *fogged or
overdrawn* it takes the engine's own texel through the palette, and only where that neighbourhood
is **clean** does it sample our `uPic` picture. `fog=13251/13356` and `13250/13356` on the two runs
above — **99.2 % of the engine's minimap is fogged**, so the clean test almost never passes and the
picture is sampled almost nowhere. `200v200`, whose units are spread across the map, reads
`13137/13356` and is no better. So these figures test the **mask** thoroughly and say **nothing**
about the picture.

That matters because the picture is exactly where the two lanes' *filters* differ — see the sampler
finding below. **A fixture with a substantially explored map is what would close it**, and none of
the ones here is that.

**So `norestore` is the only lever left** — and landing 4 below takes it, after which the pass
composites an ordinary session with nothing armed for it at all. Through landings 1 and 2 it never
did: landing 1 stood down on every frame with text on screen, and landing 2 on every frame with a
cursor on screen, which is every frame of real play.

So TA's own glyphs — rasterised by the GL lane into its own atlas, stamped from the cells that lane
resolved, coloured by the blitter's own three arguments — reproduce it exactly.

**THE CHECK THAT THE 0 IS NOT VACUOUS IS `str=`, NOT THE INK FIGURE.** A first draft of this section
argued from the ink count moving between landing 1's run and landing 2's (748 890 against 744 960)
and that argument is **wrong**: re-running the identical fixture gives 748 916 and 1 995 067, so the
figure moves between runs of the same configuration. The cause is on record — **the engine's
`PAUSED` banner BLINKS**, ~2 800 px at 1024×768 (§2.33's traps), which is the size of the swing.
Both halves of one A/B are the same frame by construction, so a 0 stands whatever the phase; what
says the strings were actually drawn is the producer's own counter, `str=38/102` with
`glyphs=27 fonts=1` — 38 string ops, 102 glyph quads. Read the counter, not the ink.

**The last row is the honest one and it is the shape of this whole pass.** The glyph serial, the
repack loss, the abandoned-frame publish, the ask cap, the quad bound and the `SET_MAX` size are all
**correct by construction and exercised by nothing here** — the fixture's glyph set is complete
before the first upload, its atlas never repacks, its store holds two twins, and its worst frame is
7414 quads. They are in the same category as §2.33's Classic++ refusal: written, argued, unfired.

**Every defect this pass has had was invisible to every measurement taken of it** — the
use-after-free, the five diverging paths, the reseed storm, the `SET_MAX` size, the glyph serial,
the repack, the withheld record, the one-shot reseed a lost frame could swallow, and three
`continue`s that had been diverging quietly since landing 1 — and every one was found by
**reading**: five reviewers across three rounds, and two passes of reading my own diff. On a pass
whose output is a pixel count that reads 0, that is what to expect rather than the exception.

**Three of those were introduced by a FIX for one of the others**, which is the real lesson of this
landing and the reason each round was re-reviewed rather than trusted: clearing `s_setView` in
`tw_drop` (worse than the hazard it closed), `behind()` reseeding the oracle every frame, and the
ask cap whose refund an ordinary session could never reach. Twice the evidence against my own fix
was already written down on this page.

**One PLAUSIBLE finding was kept rather than fixed.** GL's `uSurf` is POT-padded by the fork
(1024×512 for a 640×480 mode) while `s_engImg` is exactly the mode rect, so a `texelFetch` outside
that rect reads a defined texel in GL and an undefined one here. It cannot reach the A/B — the
composite samples inside the viewport — and closing it would mean padding our image to match a fork
detail rather than an engine one. Named rather than silently carried.

#### Landing 4 — Classic++, and the question the plan could not answer from the plan

**THE PLAN SAID THIS LANDING MIGHT BE BLOCKED ON THE RESTORER'S FIVE SHADERS. IT IS NOT, AND THE
REASON GENERALISES.** `SPR_FS` is the only producer of colour in this module and it samples
`uAtlasRGB`, the UI atlas's **restored twin** — which the Classic++ restorer paints on the GPU
(`tagpu_restoreglsl.c`, renderers.md 4c) out of five shaders G19c did not translate. Reading that as
"the port needs the restorer" is the mistake: **a second backend needs the TEXELS, never their
producer.** The restorer goes on running exactly once, in the GL context, and its output crosses as
bytes like every other input on this hand-over.

So `tagpu_gaf.c` grows a second opt-in mirror beside the indexed one, and it is deliberately **not**
the same mechanism:

| | the indexed mirror (landing 1) | the restored mirror (landing 4) |
|---|---|---|
| where the bytes come from | the CPU already has them — the decode that feeds GL feeds the mirror | only the GPU has them; a **read-back** is the only way |
| when it is written | at every paint, beside the `glTexSubImage2D` | on a frame `tagpu_rglsl_job_painted` moved, and on no other |
| arming it | marks every painted entry for repaint, or the mirror holds zeros where art is | nothing: the destination is cleared to **alpha 0** when the job is made, and alpha 0 is the restorer's own "not painted here", so a mirror allocated at any moment is behind but never wrong |
| what it costs a settled session | nothing | **nothing** — the UI atlas's restore finishes and `painted` stops moving |

**The content key is the paint count, `rgbGen` for its discontinuity, and the row bound for the
shelf.** All three are things that change what a consumer would read and nothing else is: keying on
the paint count alone misses a **re-arm** (the job is freed and counts from 0 again), and keying on
those two misses a shelf that **grew** without a paint having landed in it yet. This is landing 2's
lesson at a different atlas — a serial that is not the CONTENT's serial uploads once and then misses
everything after it — applied before it cost a round rather than after.

**The read-back sits between `tagpu_rglsl_step` and the drain**, which is where `tagpu_gui_present`
already says, in its own comment, "what it paints this frame is what the drain's sprites sample". So
the bytes handed over are the bytes the GL lane's own draws read, on the same frame — not a frame
behind them. `glReadPixels` from an FBO over the twin gives memory row 0 first (the attachment is a
texture, whose y = 0 is memory row 0, not the screen's bottom row), so there is no flip here either.

##### What the ops carry, for the fourth time on this pass

`TAGPU_GUIOP::col` is two bits of what the GL lane **did**:

* `TAGPU_GUICOL_DST` — the destination twin has a colour attachment after this op. `twin_colour`
  can refuse (no `glDrawBuffers`/`glClearBufferfv`, an incomplete FBO), so "the op wanted colour"
  and "the twin has colour" are two facts and both are needed.
* `TAGPU_GUICOL_ON` — the program's own `uRestored` (sprite) or `uSrcHasCol` (copy) was 1.

Deriving either here would be asking `s_colValid && s_atlas.rgb` a second time, of a module that
settled it in `restore_step` **before the drain**. Same rule as the sprite's atlas rect, the
string's glyph cells and the cursor's destination — and the same reason each time.

##### Where Vulkan is not GL

* **A framebuffer is immutable.** GL flips `glDrawBuffers` between 1 and 2 on one live FBO; here a
  twin that gains colour gains a **second framebuffer** over both views and a second render pass
  with two attachments. Everything else about that pass is identical — same `LOAD`, same
  dependencies — because it is the same draws writing one more output.
* **No shader was translated.** `SPR_FS`, `CPY_FS` and `STR_FS` all already declare
  `layout(location=1) out`, and landings 1–3 ran them against a **one-attachment** pass: a write to
  a location the subpass has no attachment for is discarded, which is exactly what
  `glDrawBuffers(1)` does over there. The colour edition is the same SPIR-V against a pass that has
  the attachment.
* **`twin_col_drop` is a render pass of its own.** `vkCmdClearAttachments` is the only rect clear
  Vulkan has and it needs an instance; GL does it with a scissored `glClearBufferfv` on buffer 1.
  For the same reason the CLEAR op now clears **both** attachments on a colour twin —
  `glClear(GL_COLOR_BUFFER_BIT)` clears every buffer `glDrawBuffers` named.
* **The descriptor claim is on the PAIR.** Binding 41 stopped being the dummy the moment Classic++
  arrived — a sprite samples the restored atlas there and a copy its source's colour twin — so
  keying the claim on binding 40 alone would hand a restored sprite the set of an unrestored one.
  Right indices, wrong colour image, and a picture that is **correct everywhere the colour twin
  happens to be empty**. `SET_MAX` gains two for the same reason.
* **The retire takes two object sets in ONE entry.** `fb2` names both views, so split across
  entries the sweep could destroy an attachment's view while the framebuffer naming it was alive.

##### Five places the lanes could have diverged in silence, refused instead

Every one of them writes a twin the **next** frame inherits, which is why none is a best-effort
draw: a restored sprite before the restored atlas has crossed (reachable for exactly one frame —
the pass asks for the mirror from inside its own `prepare`, so the present that ARMS the restore can
run before anything has asked); a copy whose source has colour over there and not here; the
presented twin the same way; the composite's own `uColOn`; and a `twin_colour` this lane could not
honour. The first is answered with a **fresh start** and that is not a formality — though the
mechanism is stronger than the first draft of this paragraph said, which claimed "a reseed
re-publishes every surface's bytes, and `twin_col_drop` then clears the colour on both sides".
Traced by the review: `behind()` raises `g_guiq.reseed`, the producer emits **`PK_RESET`**, and
`twins_reset` → `twin_drop` **deletes** each colour twin outright (`glDeleteTextures(1, &t->rgb)`)
while `tw_reset` retires ours. The colour is not cleared, it ceases to exist on both sides, which is
a stronger guarantee than the one this page was resting on.

And the palette re-arm — `restore_step` freeing the job and invalidating every colour twin —
is applied on a **sweep** over the store rather than at each twin's next op. A twin nothing touches
this frame is still one the composite may present.

##### THE DEFECT, AND IT TOOK A SECOND RESOLUTION TO SEE

`tagpu_rglsl_job_new` **clears its destination** when the job is made (`prepare_dest`), so every
texel of the GL restored twin above the shelf reads alpha 0. Ours is only ever written for the rows
the mirror has read back, and the rest of a 2048 square was **whatever the allocator handed us** —
which `SPR_FS` reads as restored colour wherever a byte of it clears 0.5 alpha, writes into a colour
twin, and the composite then shows.

| the same build, the same fixture, the same levers | |
|---|---|
| `selbox-slope`, 1024×768 | **0 of 786 432** |
| `selbox-slope`, 1920×1080 | **166 827 of 2 073 600** |

**The garbage was always there and only sometimes read.** The run that measured 0 had not restored
anything yet — `GL(restore)` and `GL(norestore)` were byte-identical on it — so no sprite took the
`uRestored` branch at all. One resolution, taken alone, would have called this landing done. The fix
is `vkCmdClearColorImage` over the whole image on creation, which is the GL lane's own behaviour
rather than a guard added around it.

##### Measured, with the restore ARMED and SETTLED

`gui.on=mmbase` — **no `norestore`, and no STAND-DOWN lever is left on this pass.** `mmbase` is not
one: it is a force-ON for the harness (`s_mmforce`, "draw it at k = 1 too"), which makes the GL lane
draw the sharp minimap at a scale where it would otherwise leave it to the engine, and so gives
*both* lanes more to compare rather than less. Every token that made the GL lane draw LESS is gone.
[The distinction is the review's: the sentence above the table read as a contradiction to the line
beside it.]
`tagpu_classicpp.on` is in `tagpu_opt.c`'s play-defaults table with `assets=1`, so this is what an
**ordinary session** does, and landings 1–3 stood down on every frame of one.

| | |
|---|---|
| `selbox-slope` in game, 1024×768 | **0 of 786 432** |
| `selbox-slope` in game, 1920×1080 | **0 of 2 073 600** |
| the same two with `norestore`, after the fix | **0** and **0** — landings 1–3 unregressed |
| the counters at 1024×768 | `colvalid=1 col=19/2 rgb=11 atlas=40/4096`, `sprites=847765 copies=5130 str=38/102 drawn=35090` |

**AND THE NON-VACUITY CHECK IS A THIRD CAPTURE, not a counter.** §2.34 has twice had to correct an
argument from ink counts, so this one is a picture-to-picture diff in the **GL lane alone**: the same
frame with `norestore` and without.

| what dropping `norestore` moves in the GL lane's own picture | |
|---|---|
| 1024×768 | **68 598 px of 786 432** — 8.7 % of the frame |
| 1920×1080 | **220 372 px of 2 073 600** — 10.6 % |

That is what the Vulkan lane reproduced to the pixel, and it is a claim no counter could make. It is
also the check that caught the defect above: the run where this number was **0** was the run whose
A/B was meaningless.

##### What the review and the RE-review found, and where they landed

Two reviewers at `high`, then one re-review of the fixes. **No CONFIRMED defect in the
synchronisation, the lifetime, the retire or the content key** — the arguments this section makes
about those all held when traced. What they did find:

* **The creation clear closed UNDEFINED memory and left STALE memory open.** `s_arRows` was a
  monotone high-water reset only on a dimension change, and `mirrorRgbRows` drops to 0 in two places
  that change no dimension. An upload after a shrink covered the new rows and left the previous
  fill's colours above them. A high-water records that rows were once written, never that they still
  say the right thing — which is exactly how it hid, and the 166 827 px above is the proof those
  rows get sampled.
* **THE UI ATLAS'S GENERATION WAS NEVER CHECKED ACROSS A PRESENT, AND THAT HAD BEEN OPEN SINCE
  LANDING 1.** `tagpu_gaf_atlas_put` runs INSIDE the drain, so a sprite that fills the atlas recycles
  it in the middle of the very present whose earlier sprites are already recorded with the rects it
  resolved. Landing 2 built precisely this guard for the GLYPH atlas; the sprite atlas was left
  without one for three landings. The subtlety that makes it safe is that `twins_reset` recycles the
  atlas too — so *every* RESET moves the generation, and a guard that did not forget the ops before
  one would lose every present answering a reseed and answer it with another.
* **`SET_MAX` stopped bounding its worst case the moment the claim became a pair** — a copy SOURCE
  can be claimed twice in one frame because `uSrcHasCol` is a property of the DESTINATION. Both
  reviewers, independently; the eighth such pair on this lane. `2 * TW_MAX + 3` is exact rather than
  slack: 64 copy claims, 2 sprite, 1 glyph.
* **And the RE-REVIEW found a defect inside a fix, for the third round running on this pass.** The
  whole-image clear and the row copy are two TRANSFER writes to overlapping memory recorded back to
  back, and **recording order is not execution order**: without a barrier between them the driver may
  land the clear after the copy, and every restored sprite then draws indexed against a `uRestored`
  that says otherwise, silently, for as long as the serial stands still. The fix for the stale rows
  is what made that path routine — it used to run once per image.

Three documentation claims were disproved and corrected: a fresh start DESTROYS the colour twin
rather than clearing it (the conclusion was right, the mechanism named was not); "no lever is left"
meant no STAND-DOWN lever, beside a table measured with a force-ON one; and this page's own
producer-side comment still asserted that rows past the shelf "can change no texel any op samples",
which is precisely the premise the 166 827 px killed. A fourth was a safety claim with no mechanism
behind it — the cursor's generation mark cannot fire, because `atlas_insert` answers exhaustion with
a flag and a NULL and never touches the generation.

**AND THIS SECTION WAS WRITTEN INTO §2.32 THE FIRST TIME, which is landing 1's bug wearing different
clothes.** That one put §2.34 itself below the `## 3. Known limits` header; this one dropped 128
lines about the UI pass into the middle of the shadow map's, because the anchor it was inserted
before — `#### Not covered` — occurs in three sections and the first match is not this one. Both
were caught by the same thing and by nothing else: **regenerating the wiki and reading the headings
of the page you just wrote.** A note that renders in the wrong section is not documentation, and
`git diff` shows it as an addition in the right file.

#### Landing 5 — the context switch, and the crash that was hiding behind it

**THE SWITCH IS MEASURED IN ONE PROCESS, BOTH DIRECTIONS, AND IT IS 0 PX AT EVERY STOP.** Every
figure above this was taken after a `--restart`, which starts a process, crosses the shell→game
switch on the way in, and measures the far side. This one drives the menus by hand
(`ui click SINGLE` → `Skirmish` → `Start`, and back out through `EXIT` → `MAINMENU` → `CHOICE1`)
so that both sides of both crossings are the same process:

| | |
|---|---|
| the shell, 640×480, Classic++ armed | **0 of 307 200** |
| → the switch: lane down, window destroyed, new window, new swapchain, **up in 173 ms**, one fresh start | |
| in game, 1920×1080 | **0 of 2 073 600** |
| → the switch back: down, destroyed, created, swapchain at 640×480, **up in 145 ms** | |
| the shell again, after the round trip | **0 of 307 200** |

**THE WHOLE-LANE TEARDOWN IS CORRECT AND NOT A COST TO REMOVE.** `render_ogl.c`'s render loop calls
`tagpu_vk_render_stop` on every exit, and its comment is the argument: a mode change invalidates the
HWND the Vulkan surface was made on, so the lane comes down on the thread that owns it rather than
being left to discover a dead window. This is also what the ta-drive skill measured for the world
passes in G19e ("an in-process map change brings the WHOLE Vulkan lane down and back up").

##### And the reverse crossing CRASHED, in the GL publisher, with nothing to do with this port

Quitting a skirmish to the main menu at 1920×1080 took an access violation and the process then
spun at 100 % CPU with no further output. The route is the one the skill documents as the way to do
it, and `MAINMENU.GUI 640x480` is its own stated criterion for the level having really torn down.

**IT REPRODUCES WITH `vk.on` REMOVED, and the fault is identical** — same instruction pointer, same
call stack, same illegal-read shape. That is what makes it the fork's rather than the lane's, and it
was worth an extra run to establish before touching anything.

**Finding it took one step past what TA's own crash handler says.** The handler prints "in module
TotalA.exe", but that image ends at `0x51FC00` and the IP was `0x7903BBA1`; `/proc/<pid>/maps` on
the still-spinning process puts that page on **our own `ddraw.dll`**, so it is RVA `0x3BBA1`.
Disassembling there gives `cmp BYTE PTR [edi+0x9], 0` — the exact bytes the ErrorLog printed — with
`0x811C9DC5`, this function's own FNV basis, two instructions later.

The function is `frame_key` in `tagpu_gui_hook.c`, and the defect is one line wide:

```c
if (!ptr_ok(px)) return NULL;      /* the PIXEL pointer is checked */
if (fr[0x09] == 0) {               /* the FRAME HEADER is not, and never was */
```

`fr[0x09]` is `TAGPU_GF_COMP`, and the per-level GAF bank it lives in had just been freed by the
teardown's cascade (`freed 279 block(s)`). Every other caller of the GAF resolvers in this tree —
`tagpu_fx.c`, `tagpu_feat.c`, `tagpu_render3do.c`, `tagpu_gui_surf.c` — puts the header through
`tagpu_gaf_frame_sane` first.

##### The fix is an ordering, and the obvious cheaper key does not work

A `frame_sane` on `fr` **would** have stopped this crash, because the page is unmapped. It is still
not the fix: a freed-but-still-mapped page passes `IsBadReadPtr` and returns garbage, which is the
same bug with better odds and wrong art instead of a stop. It is kept as a **bound**, and this page
says plainly that it is not the safety argument.

The safety argument is that the op carries **the level it was OBSERVED in**, and `publish` refuses
to resolve a GAF frame while a teardown is in flight, from a level that has ended, or when nothing
is tracking levels at all.

**THE "ONE THREAD" PART IS ESTABLISHED BY THE CODE, AND A PREVIOUS REVISION OF THIS PARAGRAPH SAID
IT WAS NOT.** It claimed `before_flip` checks `on_game_thread()` while "the blit observers that call
`op_add` do not". They all do: `tagpu_gui_leaves.h` gates every entry point that can reach `op_add`
— `before_gaf`/`gafa`/`gafb` (:102,103,106), text (:121), line (:177), `bar`/`frame`/`rect`/`focus`
(:202,205,206,208), copy (:219), gafd (:249), scale (:270), fill (:292) — and `on_game_thread()` is
`s_gameTid != 0 && GetCurrentThreadId() == s_gameTid` with `s_gameTid` set in `tagpu_gui_init`, so it
is never vacuously true. `publish` itself runs only from `before_flip`, past the same check. The
stamp and the bump are on one thread, by construction, and this is program order rather than a claim
about visibility. [The false version was caught by BOTH reviewers of the third pass; a note that
invents a hole is worse than a missing one, because the next session goes and "fixes" it.]

**THE GENERATION IS `tagpu_packet_pub`'s, AND KEYING IT ON `tagpu_reclaim`'s WAS THE FIRST VERSION'S
REAL DEFECT.** Reclaim's generation moves only while reclaim is ARMED — and `tagpu_reclaim.off`, one
file, leaves it a constant `0` while the engine frees the GAF banks exactly as before, because the
cascade is the ENGINE's and reclaim defers only `Object3do` and model templates. The gate was
therefore inert in precisely the configuration that needs it, with `frame_sane` — the probe this
section says is not the safety argument — left as the only thing between the shipped DLL and the
crash. **Both landing-5 reviewers found it independently**, the ninth such pair on this lane.

The tree had already ruled on this class and the fix used the counter that does not move.
`tagpu_packet_pub` installs its **own** observer on the teardown `0x491B60` when reclaim is not
armed, for the stated reason that the level-end packet must not depend on another module being
armed, and `tagpu_packet_pub_level_gen`'s own header says it is what "a game-thread observer that
latches per-level state" should stamp with. `tagpu_packet_pub_level_tracked()` is the first test:
when no provider exists the generation never moves, and no ordering being available is a reason to
**refuse** the read rather than to take it.

**AND THAT PREDICATE WAS WRONG THE FIRST TIME, WHICH IS THE FOURTH LANDING RUNNING WHERE THE
RE-REVIEW FOUND A DEFECT INSIDE A FIX.** It read `s_levelEndBy != 0`, which is assigned only when the
PUBLISHER is armed — so under `tagpu_packet.off` it answered "nothing tracks levels" while the
generation was moving perfectly well, because `tagpu_packet_pub_level_end` bumps it as its first
statement, above every gate, and reclaim's wrap calls that from a hard-wired stub whether this
module is armed or not. The gate would then have refused **every** GAF op for the whole session and
degraded every UI sprite to a box upload — fail-safe, and a total silent loss of the sprite path on
the one lane whose cost is about to be measured. It is `s_levelEndBy != 0 ||
tagpu_reclaim_level_tracked()` now: either provider moving the counter is enough.

One asymmetry between the two providers is worth knowing and is **not** closed: reclaim's wrap is an
unconditional stub, so it bumps the generation on whatever thread the teardown runs on, while this
module's own observer returns early off the game thread and the generation then does **not** move
for that teardown — which fails OPEN at the gate. It is only reachable if `0x491B60` can run off the
game thread, which the review could not establish either way.

**AND THE WINDOW THIS SITE ACTUALLY SEES IS NOT THE ONE THE FIRST DRAFT DREW.** It argued that the
teardown flag covers the frees and the generation covers what follows, so neither is sufficient
alone. That is true of the flag and the generation in general and **false of this call site**:
`publish` runs only from `before_flip`, and `s_teardown` is non-zero only while that same thread is
inside `0x491B60`, which does not flip — so the closing test is unreachable from here today. The
case that is real is the **opposite** order, and it is why the generation does the work: at
`0x460635` the engine calls the teardown and pops the screen **afterwards**, flag already down, and
those ops are refused because their generation is stale. The closing test stays as belt — a flip
reached from inside a teardown would be caught by it — and is named as belt rather than as the
argument. [The reviewer traced all six `call 0x491b60` sites to establish this.]

The fallback when any test refuses is the op's own box out of **our mirror of the surface**, not out
of the asset — so the picture is unchanged and only the sprite's identity is lost.

**THE COUNTER IS THE SHAPE OF THE FIX; IT IS NOT EVIDENCE THAT THE FIX WORKS.** Two versions of this
paragraph thought otherwise. The first argued from `lost=` being flat — but `lost=` is the CONSUMER's
atlas miss, and a publisher-side refusal emits `PK_PIXELS` and never a `PK_SPRITE`, so it could not
have moved whatever happened. The second argued from `gafstale=` alone, which says how often the gate
fired and nothing about whether the crash it is aimed at would have happened. **The fault is
intermittent, so on this route every run that does not crash looks like a fix**, and a clean run
after a change is worth nothing until the rate of the thing being prevented is known. That rate was
never measured, and that — not the counter — is the real defect in how this landing was first argued.

**SO IT WAS MEASURED, ON THREE BINARIES.** The same route five times per arm — `SINGLE` → `Skirmish`
→ `Start`, into the game, then `tab tab` → `EXIT` → `MAINMENU` → `y`, with the shell's 640×480 read
back as the proof that the level really tore down — at 1920×1080 with the **Vulkan lane DOWN**, which
is the configuration the crash was first seen in:

| arm | the `frame_sane` bound | the ordering | crashed |
|---|---|---|---|
| **A** — neither, i.e. the landing-4 tip | out | out | **4 of 5** |
| **B** — the ordering ALONE | out | **in** | **0 of 8** |
| **C** — what ships | **in** | **in** | **0 of 5** |

**Arm B is the one that carries the claim, and it is why the bound is not the argument**: with
`frame_sane` compiled OUT and only the ordering standing, the gate refused the reads at the teardown
and nothing crashed in **eight** runs, against a base rate of four in five — `0.2^8 ≈ 3×10⁻⁶` if the
ordering did nothing. It was run to eight rather than five for a second reason: with no bound in the
build, arm B is also the probe for the SCREEN-POP window below, which the gate provably does not
cover.

**Arm A's four crashes are one fault, not four.** Every one of them: `80 7f 09 00` at the
instruction pointer — `cmp byte ptr [edi+9], 0`, which IS `fr[0x09] == 0` — at the same EIP, with
the faulting address exactly `EDI+9` and reading `0x09A48D21` every time, because the per-level bank
comes back at a fixed address and only the timing of the race varies.

And the fifth arm-A run, the clean one, has an explanation rather than being noise: on arm C the
counter reads `gafstale=0` on one run in five as well, and **a run with nothing straddling the
boundary is a run that would not have crashed unfixed either.** Two fractions of one-in-five
agreeing is n=1 against n=1 and is called a coincidence worth noting, NOT a positive control — the
third review pass asked for that word back and was right to. What carries weight is arm B: 0 of 3
against a base rate of 0.8 is p ≈ 0.008 on its own.

The counts differ between arms — arm B refused 147 where arm C refuses 215 or 225 — and that is
run-to-run spread, not a difference between the builds: arm C alone ranges 0 to 225 across its five.
How many ops are straddling the boundary depends on where in the census window the teardown lands.

The shape inside one arm-C run, which is what the counter is actually good for:

| | |
|---|---|
| in play | **`gafstale=0`** on 30 heartbeats while `sprites=` climbs 9 831 → 534 525 |
| `packet: level end -> gen 1 (reclaim's 1)` | 6462 in-play draws that level |
| after the teardown | **`gafstale=215`** on 18 heartbeats — and **no other value all session** |

Across the five arm-C runs: 215, 225, 0, 225, 215. It steps once, at the boundary, and those are
reads that used to go into freed memory.

**THE TRAP THAT COST A ROUND OF THIS: THE ARMS WERE FIRST RUN WITH THE VULKAN LANE UP, AND ARM A
READ CLEAN 1 OF 1 THAT WAY.** Route D draws a second window's worth of work in the same iteration of
`ogl_render`, so the publisher's queue drains on a different schedule and the race closes. A crash
reproduction has to run in the configuration the crash was seen in, and "the lane is off" is part of
that configuration even when the bug has nothing to do with the lane. On the strength of those clean
runs this section had already withdrawn the `225` above as an artefact of the `s_levelEndBy` defect.
**That withdrawal was wrong** — the number reproduces at 215/225 on the corrected predicate, because
in the configuration it was measured in the publisher was armed and that disjunct never fired. The
re-review's finding stands as a latent defect under `tagpu_packet.off`; the measurement it appeared
to discredit was sound.

**THE SAME GATE NOW STANDS IN FRONT OF THE FONT, AND IT SHIPS UNMEASURED ON THE PATH IT GUARDS.**
`publish`'s `OP_TEXT` branch handed `op->frame` — the FONT object here, not a GAF frame — straight to
`gfont_slot`, which reads `f[3]` and `f[0]` with no bound, no probe and no generation, on a pointer
captured up to `CENSUS_MS` earlier. The identical recorded-before / published-after shape the rest of
this function had just closed, twelve lines below it, and **both reviewers of the third pass found it
independently** — the tenth such pair on this lane. The font is `[globals+0x204]` (`0x4B6220` is
`mov eax,ds:0x51FBD0`), so it is probably not per-level; "probably" is not a lifetime. It is gated
the same way rather than a different way, because a different rule would need an argument the font's
lifetime does not give us, and a refusal falls through to the box's own bytes — what this path did
before G17d, so the picture is unchanged. The refusal is counted as **`strstale=`** and NOT folded
into `gafstale=`, because the A/B above is stated in that counter and a second reason inside it would
make those numbers mean something else on the next run that reads them. **Both counters are gone
since**: `gafstale` with landing 7 and `strstale` with landing 8, each when the gate it counted
stopped existing — the figures quoted in this section belong to the builds they were measured on.

**What could not be measured, and it is a gap that predates this gate.** The consumer's `str=`
counter — strings actually drawn and mirrored — read **0/0 on every fixture driven for this landing**:
in game at 1920×1080 under `gui.on=mmbase` and under landing 2's own `gui.on=nocursor nominimap
norestore`, and in the shell on `SKIRMISH.GUI` at 640×480. The A/B reads 0 px in all of them, and **0
px over content that is not there is not a measurement** — the lesson landing 1 already paid for. The
gate is not the cause: the control build with it reverted reads the same `0/0`, and `strstale=0` says
it refused nothing. So the change is established to make no difference and is NOT established to be
correct on a live string.

**A FIXTURE THAT DOES PUBLISH STRINGS EXISTS SINCE, and it is the walk** [MEASURED 2026-09-16,
landing 8]: the in-game `--vk` walk reads **`str=` 6 281–6 532 strings / 30 291–31 537 glyph
quads**, `miss=0`, `fonts=1`, 27 glyph cells. Launching a skirmish and letting it sit still reads
`str=0/0` with the same levers and `cpp=1` either way, so it is **what the walk drives** and not
the arming; which of the walk's actions reaches `DrawTextCustomFont 0x4C14F0` is not established
and does not need to be for the figure to be usable.

**AND `glyphs=` CLIMBING DOES NOT MEAN A STRING WAS DRAWN**, which is the trap that made the
sitting fixture look like it had no strings at all. In that run `glyphs=` reached 16 cells while
`str=` stayed 0 — and `glyph_raster` has exactly one caller, `tagpu_text_glyph_feed`, so the
string ops *did* arrive. The feed sits in the drain loop **above** the skip gate and runs whatever
the switch decides; the stamping is `case PK_STRING`, which needs `twin_find(o->surf)` to find a
twin and needs the op not to have been skipped to a reset. `miss=`, `reseed=` and the
"stamped nothing" line were all 0, so `twin_string` was never entered. Read `str=` for "text was
drawn" and `glyphs=`/`fonts=` for "the records arrived"; they are different questions.

**AND THAT PUTS A QUESTION MARK ON LANDING 2's OWN NON-VACUITY.** Landing 2 reports "0 of 786 432 at
1024×768 and 0 of 2 073 600 at 1920×1080, with strings ON" on that same lever set, and the run here
at 1080p on that lever set published no string at all. The map and the instance differ, so this is a
question and not a refutation — but the next work on this pass needs a fixture that DEMONSTRABLY
publishes one (`str=` climbing, not merely `nostring` being absent) before any string figure on this
page is believed, this gate's included. The only session today that read a non-zero `str=` had walked
`RENDER.GUI` and `VISUALS.GUI`, which are our OWN injected menus and a different text route.

**This is the standing debt in `CLAUDE.md` coming due, once.** The roadmap already lists "the
per-LEVEL ASSET class under `tagpu_reclaim`'s fence" as an open row of the frame-packet gate; this
landing closes the one site of it that the UI publisher owns, by ordering rather than by waiting for
the asset channel, and leaves the class itself open.

#### Not covered

* **The gate, which is the whole of G19f and not these landings.** `uiwalk`'s `strict` walk over
  the **full screen inventory** at 1024×768 and 1080p, shell and in game; and **frame time no worse
  than GL**, which nothing in Phase G has measured at all. The shell↔game context switch is closed
  **for the crash that was measured** — the teardown cascade — and the SCREEN-POP window inside the
  same route is open, covered only by the bound, and written up below. These are a handful of fixtures, not an inventory.
* **THE PER-LEVEL ASSET CLASS ITSELF STAYS OPEN.** Landing 5 closes the one site of it the UI
  publisher owns, by ordering. The class — model templates, FeatureDef and wreck records, the GAF
  banks, all under `tagpu_reclaim`'s fence until the asset channel — is the frame-packet gate's own
  row and nothing here touches it. The only reason this one was found is that a fixture happened to
  quit a skirmish to the main menu. The landing-5 review swept the rest and **most are sound** — the
  feature, unit, terrain, effects and packet-publisher readers all sit behind the level-end packet's
  own `in_game` gate, which is published before the flag drops — but it named **two that are not**:
* **THE SCREEN POP IS THREE INSTRUCTIONS INSIDE THE ROUTE THIS LANDING MEASURED, AND THE GATE DOES
  NOT COVER IT.** This bullet used to file `GUI_Pop 0x4A9660` away as "the shell's route", which is
  true and misleading. The disassembly of the site the landing itself cites:

  ```
  460630:  call 0x491b60     ; the teardown -- bumps gen N->N+1, lowers s_teardown
  460635:  push 0x1
  460637:  call 0x491d70     ; UpdateIngameGUI -- DRAWS, so op_add stamps lgen = N+1
  460641:  add  eax, 0x519
  460647:  call 0x4a9660     ; GUI_Pop -- frees that screen's art
  ```

  The pop happens **after** the bump, so ops recorded at `0x460637` carry the CURRENT generation.
  At the next flip `level_tracked()` is 1, `level_closing()` is 0 and `op->lgen == gen()` — **the
  gate passes**, and the only thing between `frame_key` and freed art is `frame_sane`, the probe
  this section says twice is not the safety argument. `GUI_Pop` frees from **39** call sites with no
  flag and no generation, and `frame_key`'s own comment has recorded for some time that the shell
  hands those same addresses to the next screen. (The "21 event-handler call sites" this bullet
  carried until the re-review is `UpdateIngameGUI 0x491D70`'s figure, borrowed for the wrong
  function; `call 0x4a9660` appears 39 times in the pristine exe and `call 0x491d70` 21.)

  **Arm B is the experiment that says whether it is live, and it says probably not — which is not
  the same as no.** Arm B is the build with `frame_sane` compiled OUT and only the ordering standing,
  so an unguarded read faults instead of being swallowed. **8 runs of the route, 0 crashes**, against
  arm A's 4-in-5. If the ops recorded at `0x460637` named art the pop frees, arm B would have taken
  the access violation. So on THIS route, with THIS fixture, those ops do not appear to name popped
  art — evidence, not proof, and it says nothing about the shell's own screen-to-screen pops, which
  no fixture here drives at all.

  **The by-design fix exists and is not built.** The module already observes the engine's single
  free — `before_memfree` on `MEM_Free 0x4D85A0`, which every one of the engine's 363 free sites
  calls, on the game thread, at the entry before the allocator's own critical section — and already
  uses it to retire surface-table entries by block. A block-keyed forget for queued ops would cover
  the teardown cascade and the pop together and would not need a generation at all. What it needs
  first is a size: `MEM_Free` is handed the block, not its length, so "is this op's frame inside
  this block" is not answerable from the observer as it stands. [Raised by the third review pass,
  2026-09-16. Named here rather than built because it is a mechanism, not a fix to this landing's
  defect, and it wants its own measurement.]

* **`tagpu_render3do.c`'s unit atlas is never dropped at a level boundary.** Its entries key on the
  template's texture-frame ADDRESS, and the atlas resets only when full or on a context loss —
  where `tagpu_fx.c` and `tagpu_feat.c` both call `tagpu_gaf_atlas_forget` for exactly the recycled-
  address reason. A second level handed the same address would be served the first level's texels,
  and nothing would detect it. Not this landing's to fix, and it is wrong art rather than a crash.
  (The GL UI atlas is safe here: `publish` stores a CONTENT hash as the identity, so a recycled
  address cannot alias.)
* ~~The cursor, the minimap and the sharp layer~~ **CLOSED by landing 3.** `norestore` is the only
  lever left.
* **THE MINIMAP'S PICTURE PATH (`uPic`), and with it the only LINEAR sampler in this module.** The
  GL texture is `MIN_FILTER = GL_LINEAR, MAG_FILTER = GL_NEAREST` and the Vulkan lane matches it
  with a second sampler — **which took two attempts, the first being a no-op** (see above) — but no
  fixture here reaches that branch of `MM_FS` (99.2 % fogged), so the match is **correct by
  construction and unmeasured**. That combination is exactly how the first attempt survived: nothing
  the pass measures would have changed had the sampler stayed wrong. It is the same category as §2.33's
  Classic++ refusal. Closing it needs a fixture with an explored map.
* **The sharp layer's THIRD client, the sharp string path**, is not exercised by any fixture here:
  `SHARP_TEXT` draws at device resolution where `twin_string` stamps into the twin, and nothing in
  `selbox-slope` takes it. The quads would cross like any other, but that is an argument and not a
  measurement.
* ~~Classic++ colour twins and the MRT sprite/copy programs~~ **CLOSED by landing 4**, and it was
  **not** blocked on the restorer's five shaders: they go on running in the GL context and their
  output crosses as bytes. **No lever is left on this pass.**
* **THE RESTORE'S OWN COST, which this landing pays and does not measure.** The read-back is a
  `glReadPixels` of `2048 × (shelfY + shelfH) × 4` bytes on every frame the restorer painted, and it
  **synchronises** — it is issued right after the paint draws. A settled session pays nothing (the
  UI atlas's queue drains and `painted` stops moving) and an unarmed one pays nothing at all, but
  the frames DURING a fill are not counted anywhere and no figure here bounds them. It belongs to
  landing 6 with the rest of the cost question.
* **THE READ-BACK CHANGES THE ORACLE LANE'S TIMING, AND "THE GL LANE IS UNCHANGED" IS THEREFORE NOT
  LITERALLY TRUE WHILE IT IS ARMED.** `glReadPixels` is synchronous, and the restorer slices its
  work against a GPU-time budget (`tagpu_restoreglsl.c`, `budget=MS`) — so with the mirror armed the
  GL lane paints a different number of batches per frame than without it. It changes no PIXEL, and
  the lane is unarmed in every session that is not running this A/B, but the claim this page makes
  everywhere else is about the lane's OUTPUT and this is the one place the distinction matters.
  [The review's, and it is the honest form of the sentence.]
* **THE UI ATLAS'S GENERATION GUARD IS UNFIRED HERE.** `mirlost=0` on every run above, and
  `atlas=53/4096` — these fixtures never fill or recycle the sprite atlas, so the frame-loss path
  the review opened is correct by construction and exercised by nothing. It is in the same category
  as the glyph atlas's own repack guard, which landing 2 added and no fixture has ever taken either.
* **A PALETTE RE-ARM IS NOT EXERCISED BY THESE FIXTURES BEYOND ITS FIRST.** `rearms=1` on every run
  — the one the shell→game transition causes — so the sweep that invalidates every colour twin has
  fired once and always with two twins in the store. The Gamma slider and `+gamma N` are what drive
  it in play and neither is in a fixture here.
* **The present itself** — landing 5. Route D still gives the Vulkan lane a window of its own.
* **Frame time**, which is half the gate's own wording — landing 6, and nothing in Phase G has
  measured cost at all.
* **The glyph atlas at dimensions this pass will not carry.** `ATLAS_MAXDIM` bounds the upload, and
  a glyph atlas outside it leaves `s_glHave` 0, which takes the replay to `standdown` and the store
  to a fresh start on every frame that draws a string. `GA_W`/`GA_H` are compile-time **512 × 256**
  in `tagpu_text.c` against an `ATLAS_MAXDIM` of **8192**, so this is a refusal that has never been
  exercised and cannot be without editing the GL lane — the same category as the Classic++ one
  in §2.33.
* `ss` 2, the owed teardown, an in-process map change, and the validation layer still.

### Landing 6 — the frame-time harness, and why its ratio does not answer the gate

`tagpu_ftime.c/.h`, armed by `tagpu_ftime.on`, inert without it. Two GPU timestamps per lane per
frame, a 256-frame ring each, p50 and p99 rather than a mean, nothing blocks: the GL side polls
`GL_QUERY_RESULT_AVAILABLE` and carries a frame whose pair is not ready, the Vulkan side is read
behind the fence the seam already waits on before it re-records that slot.

**TIMESTAMPS ARE FORCED, NOT PREFERRED.** `GL_TIME_ELAPSED` is scoped, only one may be active per
target, and `tagpu_restoreglsl.c` already runs one around every restorer slice — a frame bracket of
that kind would either fail to begin or break the restorer's. `glQueryCounter` has no such rule and
is what the Vulkan half does anyway (`vkCmdWriteTimestamp`).

**THE HEADLINE THIS SECTION FIRST CARRIED — "the Vulkan lane's frame costs 0.56–0.66 of the GL
lane's, the gate met with margin" — IS WITHDRAWN.** It was measured, it was reproducible within a
run, and it was wrong about what it measured. What follows is what the harness actually establishes,
which is less than the gate wants and more useful than nothing.

#### What it measures: an ELAPSED SPAN, and the two spans are not the same

`glQueryCounter` records when the GPU **reaches that point in the command stream**, so the delta is
elapsed time on the GPU timeline and counts anything that stalls inside the bracket — the render
thread's own `EnterCriticalSection` three lines after `gl_begin`, the restorer's synchronising
`glReadPixels` — whether the GPU was working or idle. The two brackets then span different things:
GL's runs the whole CPU frame (top of `ogl_render` to just before `tagpu_vk_frame`), Vulkan's only
its own command buffer (`TOP_OF_PIPE` after `vkBeginCommandBuffer` to `BOTTOM_OF_PIPE` before
`vkEndCommandBuffer`). That difference is survivable only on a frame where the GPU is saturated
throughout. It is not survivable here.

#### The measurement that withdrew the claim

One binary, one fixture (`200v200`, `--maxfps 0`), the sim **paused at a fixed tick** (~950–975,
`alive` 381–387), the pause verified by peeking `*0x511DE8+0x38A47:4` twice:

| resolution | MP | `gl p50` | `vk p50` | **vk/gl p50** | fps |
|---|---|---|---|---|---|
| 640×480 | 0.31 | 5.501 ms | **0.052 ms** | **0.010** | 166.6 |
| 1280×720 | 0.92 | 156.650 ms | 93.966 ms | **0.600** | 6.2 |
| 1920×1080 | 2.07 | 118.840 ms | 105.281 ms | **0.886** | 7.8 |

**THE RATIO IS NOT A PROPERTY OF THE LANES.** It moves 0.010 → 0.886 across resolutions on the same
binary and the same paused scene, and it moved 0.556 → 0.886 at 1920×1080 between two builds that
differ only by this landing's review fixes. A figure that swings by 90× is not "what a Vulkan frame
costs relative to a GL frame".

**`vk p50` CANNOT BE THE LANE'S OWN WORK.** 52 µs at 640×480 against 105 ms at 1920×1080 is a factor
of ~2000 for 6.75× the pixels. 52 µs is a believable command-buffer cost for this scene on this GPU;
105 ms is not. What the bracket picks up at the higher resolutions is the command buffer **waiting
for a GPU the GL lane is saturating** — route D runs both lanes in one iteration of `ogl_render`, so
they contend, and `TOP_OF_PIPE`→`BOTTOM_OF_PIPE` spans a queue stall exactly as it spans work.

**AND THE COST IS NON-MONOTONIC IN RESOLUTION.** 1280×720 is about twice as slow as 1920×1080 with
2.25× FEWER pixels, reproduced on one binary. This is an unexplained anomaly of the GL lane and is
worth its own investigation; it is flagged here and not diagnosed. It also disposes of the argument
an earlier revision of this section made — that 6.75× fewer pixels giving 10.7× the frame rate
established a fill-bound frame. Those were two points on a curve that does not run that way, and the
landing-6 review had already challenged the inference on principle (a resolution-DEPENDENT CPU cost
produces the same scaling with the GPU idle) before the third point showed the premise was not
merely unproven but false.

#### What the harness is still good for

* Within one paused scene it is precise: run A's absolute figures drifted 13 % between two
  consecutive report windows while the ratio repeated to three decimals.
* It is the first instrument on this lane that reports per-frame GPU-timeline cost at all, and it
  found the 720p anomaly on its third run.
* The GL half under-samples at high frame rates and this matters when reading it: at 166 fps it
  harvested **296 of ~5300 frames** against the Vulkan half's 4850, because `GLQ` is 8 pairs and a
  frame that finds none free is skipped. The two lanes' percentiles are then computed over very
  different samples of one session, and the GL sample is selected for frames where the driver had
  caught up.

#### Not covered

* **THE GATE'S FRAME-TIME CLAUSE IS NOT ANSWERED and goes back to OPEN.** Answering it needs a method
  that does not put both lanes on one GPU in one iteration — each lane measured alone in its own run,
  or per-pass timing rather than per-frame. That is a decision about what the measurement IS, not a
  fix to this code.
* **THE 1280×720 ANOMALY IS NOT DIAGNOSED.** ~2× slower than 1080p at 2.25× fewer pixels, on the GL
  lane, on this fixture.
* **THE RESTORE'S READ-BACK IS STILL NOT SEPARATELY MEASURED.** It is a synchronising `glReadPixels`
  inside the GL bracket and a candidate for both the absolute figures and the anomaly.
* One fixture, one map, one scene, paused.

### Landing 7 — the two holes the landing-5 sweep named, closed by moving the read

Landing 5's review swept the module for the per-level asset class and named two sites it could
not close. This landing closes both. Neither is about Vulkan: they are defects in the shipped GL
renderer that the port's fixtures found.

#### The sprite: resolved where the engine proves it alive

**The publisher took a sprite's identity hash AND its decoded plane out of engine memory at
publish time**, up to `CENSUS_MS` after the blit that recorded the op. Two engine routes free
that memory inside the window:

* the level teardown's cascade — which landing 5 closed with a generation ordering, at the cost
  of refusing 215 ops at a measured level end;
* **`GUI_Pop 0x4A9660`**, which frees a popped screen's art from **39** call sites with no flag
  and no generation, and which landing 5 could **not** close. The disassembly is why:

  ```
  460630:  call 0x491b60          ; the teardown -- bumps gen N->N+1, lowers s_teardown
  460635:  push 0x1
  460637:  call 0x491d70          ; UpdateIngameGUI -- DRAWS, so op_add stamps lgen = N+1
  46063c:  mov  eax, ds:0x511de8
  460641:  add  eax, 0x519
  460646:  push eax
  460647:  call 0x4a9660          ; GUI_Pop -- frees that screen's art
  ```

  The pop is **five** instructions after the bump, so ops recorded at `0x460637` carry the
  current generation and the gate passes. Only `frame_sane` stood there, and a bound is not the
  safety argument. (**This listing said "three instructions" and silently omitted `0x46063c` and
  `0x460646` until both landing reviewers counted them, independently.** The count changes
  nothing about the argument — the pop is after the bump either way — but a listing that reads
  as contiguous and is not is exactly the kind of note that costs the next person an hour.
  `call 0x4a9660` appears **39** times in the pristine `.text`, `0x460647` among them; re-counted
  with the correction.)

**Both reads moved into `gaf_box`, the `before_` detour on the blit leaf**, and the ordering is on
the engine's timeline:

> our read  <  the engine's blit  <  the engine's free

Every free route that opened this window — the cascade, and all 39 pop sites — runs **after** the
blit it follows, and the caller holds the art alive across the call it is making. Our read precedes
that call. It needs no flag, no generation and no `MEM_Free` block size, and it covers free routes
nobody has found yet, because it does not enumerate them: it is earlier than all of them. After it,
`publish` dereferences **no engine asset memory on this path at all** — the `gui probe:` trace
included, which was the last one left. `frame`/`pix` survive in the op as the consumer's atlas KEY,
a value compared against a table and never followed.

**It is NOT the claim that "the engine would fault if this were dead".** This section said that
until the landing review; the detour runs *before* the engine's read, so if the memory were dead
**we** would fault first. That was a counterfactual dressed as a proof, and it is not what makes
the change safe.

**AND IT BOUNDS LIFETIME, NOT EXTENT.** The engine reads the **clipped sub-rect**;
`tagpu_gaf_decode` reads all `w*h`, or every RLE row. A header whose `w`/`h` exceed the plane the
loader actually allocated is therefore covered by nothing above — only by `tagpu_gaf_frame_sane`,
a SHAPE test (`w,h <= 512`), and the decoder's own `IsBadReadPtr`, which this note says everywhere
is not a safety argument. **That residual is unchanged by this landing**: the same decode read the
same bytes at publish before it, over memory that might *also* have been freed. Moving it removes
the lifetime half and leaves the extent half exactly where it was. Bounding it needs the plane's
allocated length, and the plane is not a block start, so `MEM_Size` cannot answer it either.

**It also fixes a wrong-art case the generation could not see.** The key is a hash of the plane's
first bytes precisely because the shell hands a freed screen's addresses to the next screen's art.
Taken at publish time, that hash read whatever the address held *then* — so art freed and replaced
inside one census window hashed the **new** content under the **old** op, and the consumer matched
a key naming pixels the op never drew. Taken in the observer, it is a hash of the bytes the engine
is about to blit, which is the only content the op ever meant. Nothing measures this case; it is
an argument from what the two versions read, and it is stated as one.

**The generation gate is removed from this path** rather than kept as belt. It guarded the two
reads that moved, could never cover the pop, and was not free. `gafstale` went with it — the name
changed with the meaning on purpose, because `gafstale=215` is the figure landing 5's A/B is
stated in and a counter that keeps its name while measuring something else is how those numbers
quietly stop meaning what these notes say they mean. **The `OP_TEXT` path kept its gate and its
`strstale` through this landing**, `gfont_slot`, `glyph_block_size` and `glyph_block_fill` still
reading the font object at publish, and `op->lgen` existing for them alone — **closed the same
way one landing later; see Landing 8 below.**

#### The UI atlas: dropped at the level boundary too

**Found by the landing review — both reviewers, independently, the eleventh such pair on this
lane.** `tagpu_gui_surf.c`'s UI atlas matches entries on `(o->frame, o->pix, fw, fh)` — the
frame's **address** and its content hash — and its only resets are `twins_reset`, the atlas
filling, and a GL context loss. **None of those is a level boundary.** The engine frees a level's
GAF banks and the next level's loader may hand a new frame an old one's address; `frame_key`
hashes only the plane's first 64 bytes plus the hotspot, so **UI art whose first RLE row is one
transparent run can collide by CONSTRUCTION**, not by 2^-32 luck — and then `atlas_find` hits the
old entry and the twin draws the previous level's texels, with no counter moving.

**The gate this landing removed was never the cover for that**, which is worth saying plainly
because it is the obvious thing to assume: `op->lgen != level_gen` refused ops **recorded** before
a boundary and **resolved** after one — a ~5 ms window — and did nothing whatever about entries
already sitting in the consumer's atlas from the previous level. Those survived it. So the hole
predates this landing and the fix is a **drop**, not a refusal.

The publisher raises a reseed when the level generation moves, and the machinery is already
there: `PK_RESET` makes the consumer call `twins_reset`, which calls `tagpu_gaf_atlas_reset`, and
the UI atlas never asks for a repack, so that goes straight to `atlas_drop` — every entry, every
hash. Same shape as the unit atlas's, one level down the stack.

#### The unit atlas: dropped at the level boundary

`tagpu_render3do.c`'s atlas matches entries on the frame header's **address** and the pixel
plane's (`tagpu_gaf_atlas_find`), so an entry is right only while that address means that art. The
engine's teardown frees the model textures and the next level's loader may hand a new frame an old
one's address — at which point the atlas serves the **previous level's texels** and nothing
detects it: the entry is valid, the UV is in range, the picture is simply wrong. It reset only
when FULL or on a GL context loss, and neither is a level boundary. `tagpu_fx.c` and
`tagpu_feat.c` both already drop theirs here for exactly this reason.

**This was live in ordinary play**, not only under a lever: `tagpu_native.on` is in
`tagpu_opt.c`'s play-defaults table as `"all wrecks"`, so every play session has the unit atlas
up, and any session that played a second level was exposed.

`tagpu_r3d_atlas_level` is called from `tagpu_native_frame` beside `cache_gen_check` and **before**
`tagpu_posebake_frame`, not from `tagpu_r3d_atlas_frame` lower down the file: posebake **latches**
`tagpu_r3d_atlas_gen()` for the whole frame, so a drop after it would stamp this frame's bakes
with the generation before the drop and cost a second, pointless drop on the next frame. Taken
where it is, every consumer sees one generation per frame.

#### What was measured

| claim | how | result |
|---|---|---|
Everything below is from the binary that lands — the whole set was re-run after the review's
fixes, because those fixes changed the DLL and a figure off an earlier build names a build that no
longer exists.

| claim | how | result |
|---|---|---|
| the picture does not move | the A/B walk, all three resolutions | **52 of 52 stops at 0 px**; the 1024×768 and 1080p walks are identical to the pre-change runs **stop for stop, to the byte** |
| the crash route is clean | the landing-5 route, 5x at 1920×1080, **Vulkan lane DOWN** | **5 of 5 clean**, `teardowns=1` each |
| the scratch bound holds | `gafscratch=high/lost/baddec` | high-water **860 849 bytes of 2 097 152 (41 %)**, **`lost` 0, `baddec` 0** |
| nothing falls back for a real reason | `gafnoplane` | **0**, over every run |
| **the UI atlas** drops at each boundary | two skirmishes in ONE process, `gui.on=...log` | **`gui: reset #3: level-changed` and `#6`** — exactly two, one per level end |
| **the unit atlas** drops at each boundary | the same session | **2 resets**, logged `subject replaced` (our call, not a full-atlas recycle), each one line after the UI atlas's |
| a dropped atlas re-decodes the RIGHT texels | `glshot` on level 2, after the drop | units render with their own textures and shadows; terrain, trees and HUD intact |

**One stop of the 52 is not reproducible between runs and it is not this landing's.** `ARMMAIN2`
at 640×480 — the first in-game stop, the one closest to the scenario load — read 103 153 px of
ink in one run and 103 376 in the next, a 223 px difference. The same stop is **identical across
all three 1024×768 runs** (118 946 every time), so it is not the review's fixes, and both lanes
agreed exactly in both runs, so it says nothing about either. The cause is **not established**;
it is recorded rather than explained, because the ink column is evidence about what the engine
drew and an unexplained wobble in it is worth a line even when the diff it guards reads 0.
**[SETTLED as far as it can be, 2026-09-16, landing 8]**: the same walk run TWICE on one binary
reads 103 074 and then 103 376, so the wobble is the fixture and no binary difference produces
it. The cause is still unknown; the question "is it the change?" is closed.

`gafreseed` is counted apart from `gafnoplane` and is **not** a failure: a reset clears the seen
table after an op has already decided it needs no plane. It is **cheap rather than free** — the
word this section used until the review corrected it: each one costs a `PK_PIXELS` box in the
arena and one window without its atlas identity, in a publish that is already re-seeding every
surface whole, so nothing is on screen that would not have been, and it self-heals next window. It read **4 323** over the three
walks. One counter for both would have read as 3923 failures on the first
session that measured it — which is exactly what it did read before the split.

#### Not covered

* **THE FONT WINDOW WAS STILL OPEN AFTER THIS LANDING** — the `OP_TEXT` path still
  dereferenced the font object at publish, covered by the level generation and by `ptr_ok`, neither
  of which is a lifetime. It was named here as an open window rather than left to look closed by
  the line above it in the source, and **Landing 8 closed it** by the same move.
* **The wrong-art case the move fixes is an argument, not a measurement.** It needs a pop and a
  reload inside one ~5 ms census window, and no fixture here forces that.
* **The A/B cannot see this class at all**, and that is worth saying plainly: both lanes consume
  the same published op stream, so a publisher that resolved the wrong art would hand both lanes
  the same wrong art and score 0 px. The walk is the regression gate for this landing, never its
  evidence.
* **The unit atlas's drop is evidenced by the log line and a picture, not by a diff against the
  bug.** Forcing the address reuse the fix exists for would need the second level's loader to be
  handed a specific block, which nothing here can arrange.
* `MEM_Size 0x4D8360` exists and would have made the block-keyed forget buildable; it was not
  used because it reads the heap outside the allocator's own critical section. See
  [exe-reverse-engineering](exe-reverse-engineering.html).

### Landing 8 — the font window, closed the same way

Landing 7 moved the sprite's two reads into the observer and said, in the source and here, that
the `OP_TEXT` path still read the font object at publish and that a level generation was standing
in for a lifetime it did not have. **This closes that**, by the same move, and it is the last
per-level engine asset `publish` dereferenced. Like landing 7 it is not about Vulkan: it is a
defect in the shipped GL renderer that the port's fixtures found.

#### What moved

`gfont_slot` and the walk that turns a string's unsent codes into glyph records now run in
`before_text` — the detour at the head of `0x4CCF60`, the engine's glyph blitter — with the
engine's own `font` and `str` arguments in hand. The op carries what the publisher needs and
nothing it would have to follow: `fid` (the slot id the consumer's glyph cache keys on), `gboff`/
`gblen` (the records, in a 128 KB window scratch beside the sprite's 2 MB one), `frows`/`fyoff`
(the two header bytes the consumer stamps quads with) and `fgen` (below). `publish` copies our own
bytes out of our own scratch.

**Two publish-time walks became one observe-time walk.** `glyph_block_size` sized the block and
`glyph_block_fill` wrote it; with a scratch that can be bounded per record, sizing first buys
nothing. `glyph_block_capture` writes, and `glyph_block_mark` — which reads no font, only our own
records — marks them sent at publish.

#### The ordering, and why it is a stronger argument here than on the sprite

The detour sits at the head of the blitter with its arguments, one instruction before it walks
that string through that font, so

> our read < the engine's read < any free of the font

holds by the engine's own sequencing. **It is not "the engine would fault if this were dead"** —
the detour runs first, so we would fault first; that phrasing was a counterfactual dressed as a
proof when the landing-7 review found it on the sprite path and it is no better here. What makes
the read safe is that the engine has already committed to making it.

**And it bounds EXTENT as well as lifetime, which the sprite's move did not.** `0x4CCF60` has no
clip and no destination bound at all — it writes `sum(widths) × font[0]` pixels wherever the
caller said, and it cannot skip a glyph's bits because the destination would be off-screen. Our
walk takes the blitter's own two skips (`sub ebx,first; jb` at `0x4CCFAA`, `or ebx,ebx; je` at
`0x4CCFB9`) and reads the same bytes for a **subset** of the string's codes — the ones this font
has not sent yet. So where landing 7 closed the lifetime half and left the extent half exactly
where it found it (the engine blits a clipped sub-rect; `tagpu_gaf_decode` reads all `w×h`), this
path has no extent half to leave. The instruction-level read set is in
[exe-reverse-engineering](exe-reverse-engineering.html), "WHAT THE BLITTER READS, EXACTLY".

#### What the gate cost, and what replaced it

**Nothing replaced it.** `op->lgen` is gone from the op and from `op_add`, `strstale` is gone, and
`tagpu_packet_pub_level_tracked` and `tagpu_reclaim_level_closing` now have **no caller in the
tree** — kept rather than deleted, because they are the level-lifetime API those modules expose
and removing them is not this landing's business.

**What did have to be added is a generation, and it is not the level's.** The block omits the
codes the font has already sent, and *that* decision is the only thing between the capture and the
flip that can go stale. Two things clear a `sent[]` table: the render thread throwing its glyph
atlas away (a shelf overflow or a ninth font, `gfont_check_gen`), and a publisher reseed skipping
whole windows. Until this landing both were safe **by position** — the decision was taken inside
`publish`, after either clear had already happened in the same call. Now both go through
`gfont_sent_clear`, which bumps `s_sentGen`; the op stamps it at capture and `publish` publishes
its box instead on a mismatch. Same shape, and the same reason, as the sprite path's `s_seenGen`,
and counted apart as `strrearm=` for the same reason `gafreseed` is.

A **slot recycle** needs no bump of its own, but **not** for the reason this section first gave
("nothing in flight is invalidated"), which the cross-thread reviewer showed is backwards. Both
tables are eight deep, so the ninth `(font, sig)` that makes the producer recycle is also the
ninth id the consumer sees, and `tagpu_text.c`'s own `gfont_slot` answers that with
`memset(s_gf)` + `memset(s_gatlas)` + `s_ggen++` — every cell under every old id, gone. A recycle
therefore *reliably causes* a consumer clear. What makes it safe is the generation catching that
`s_ggen++` like any other, and `gfont=` now prints recycles and resends side by side so the two
moving together is a cross-check rather than a hope.

**And the poll sits beside the decision, not at the top of `publish`.** The first version of this
landing called `gfont_check_gen()` once on entry and claimed that made the test "a comparison
against NOW". It made it a comparison against the top of the publish loop, and the render thread
can drop its atlas in the middle of one: op *k*'s block omits a code because `sent[]` said it was
published, the consumer overflows its shelf and clears, and op *k* is then committed without it —
one window of a string drawn with that character dropped and the rest closed up, `miss=` counting
it. Before G19f-8 the poll was inside `gfont_slot`, one statement before the decision it guards,
so that first version had *widened* an existing window rather than closed one. It is now polled
per op, which restores exactly the old width. [Found by the cross-thread reviewer, with the
interleaving spelled out.]

#### Marked at publish, not at capture — and no cross-op dedup

`sent[]` is set by `glyph_block_mark`, after `pub_bytes` has taken the arena slot and the bytes are
in it. Marking in the capture would mark glyphs that a queue overflow, an arena overflow, an
untwinned surface or a `dedup()` drop then threw away — and a glyph marked sent but never sent is
missing from every string for the rest of the session, which is the exact failure `gfont_check_gen`
was added to prevent.

That is also why **two ops in one window that need the same unsent code each carry it**. The
sprite path can share one decode through `s_gcap` because its consumer keys on the frame and one
copy serves every op; a glyph record is only ever read out of the op that carries it, and either op
may be the one that does not get there. The duplicate is idempotent at the consumer
(`glyph_raster` returns early on a known cell) and costs arena bytes in first-sight windows only.

#### The cost, measured

`OP` grew from **80 to 96 bytes**, so `s_ops[65536]` grew from 5.24 MB to 6.29 MB; with the 128 KB
scratch the DLL's `.bss` goes from **42 163 060 to 43 343 252 bytes (+1 180 192, +2.8 %)** on a
module that already reserves 42 MB of it. Read off `i686-w64-mingw32-objdump -h` on the two builds.

#### What was measured

Everything below is from the binary that lands — the whole set was re-run after the review's
fixes, because those fixes changed the DLL and a figure off an earlier build names a build that no
longer exists.

| claim | how | result |
|---|---|---|
| the picture does not move | the A/B walk, three resolutions, the 640×480 one run TWICE | **78 of 78 stops at 0 px** (2 × 26 shell + in game at 640×480, 13 in game at 1024×768, 13 at 1920×1080) |
| …and not merely at 0 px | the ink column against the pre-change binary | **1024×768 and 1080p identical STOP FOR STOP, TO THE BYTE**; the 640×480 exception is below and is the fixture |
| the string path is actually live | `str=` | **26 788 string ops / 129 288 glyph quads** over the four walks, `fonts=1`, 27 glyph cells |
| no glyph goes missing | `miss=`, `reseed=`, `repack=`, the "stamped nothing" log line | **0 of each**, over all of it |
| **the re-arm guard fires, and costs nothing** | `strrearm=` | **6** at 1080p (0 on the other three): six text ops published their box because `sent[]` was re-armed between their capture and their flip — and `miss=` stayed **0**. The same six appeared on the pre-review binary, so it is the walk's own 1080p shelf pressure and not a flake |
| the scratch bound holds | `glyscratch=high/lost` | high-water **7 632 bytes of 131 072 (5.8 %)**, `lost` **0** on every run |
| nothing regressed in the sprite half | `gafnoplane` / `gaflost` / `gafbaddec` | **0 / 0 / 0** |
| the arena takes the duplicates | `overflows=` | **0** — the per-op blocks a first-sight window now duplicates cost arena bytes and overflowed nothing |
| the crash route is clean | the landing-5 route, 5× at 1920×1080, **Vulkan lane DOWN** | **5 of 5 clean**, `teardowns=1` each |
| landing 7 still holds | two skirmishes in ONE process | `gui: reset #3: level-changed` and `#6`, and the unit atlas's **2** `subject replaced` resets (generations 4 and 9) |

**`strrearm=6` is the line to read twice.** It is the only new failure mode this landing creates —
a block that omits codes because they were "already sent", published after something cleared that
table — and it is why the generation exists rather than being argued away. Six of those happened
in each 1080p walk, all six published their box instead, and no glyph was missed. Without the
stamp those six strings would have drawn with characters dropped and the rest closed up, which is
the failure `gfont_check_gen` was added for in the first place.

**THE 640×480 WOBBLE IS THE FIXTURE, AND THAT IS NOW ESTABLISHED RATHER THAN ASSUMED.** Landing 7
recorded `ARMMAIN2` at 640×480 as varying between runs and could not say why. Running that walk
**twice on this one binary** settles the question the only way it can be settled: `ARMMAIN2` read
**103 074** in the first and **103 376** in the second — the same two values the pre-change and
post-change runs had produced, so a figure that moves between two runs of one build cannot have
been caused by the change. `ARMCOM1` reads 93 688 in three of the four runs on record and 94 312
in one (on the pre-review binary), which is what `ARMCOM1-back` — the same screen visited again —
reads in all four. Both lanes agree to the pixel at every one of them, and both stops are
identical to the control at 1024×768. The CAUSE is still not established; what is established is
that it is not this landing, and not landing 7's fixes either.

#### Not covered

* **The A/B cannot see this class at all**, exactly as in landing 7: both lanes consume the same
  published ops, so a publisher that resolved the wrong glyphs would hand both the same wrong
  glyphs and score 0 px. The walk is this landing's **regression gate**, never its evidence. What
  is evidence here is `miss=` — a consumer-side count of glyphs the cache refused that the engine
  would have drawn — and the ink column against the control.
* **The wrong-font case the move fixes is an argument, not a measurement.** A font freed and
  reloaded at the same address inside one ~5 ms census window would, before this, have had its
  new header read under the old op; nothing here forces that, and the shell — where address
  recycling is routine — draws no strings at all.
* **THE SHELL DRAWS NO STRINGS**, which bounds what the 26 shell stops say about this landing:
  `0x4CCF60` is reached only through `DrawTextCustomFont 0x4C14F0`, so `glyscratch` is 0 across
  the whole shell half and those rows are a regression gate on the rest of the module. Everything
  this landing changes is in game.
* **A STRING LONGER THAN 256 BYTES IS TRUNCATED BY US AND NOT BY THE ENGINE** [named by the
  landing review, 2026-09-16]. `0x4CCF60`'s walk has no counter — it draws to the NUL or the
  `'\n'`, however long that is — while `before_text` measures the op's box from the first 256
  widths and `twin_string` stamps at most 256 quads. So a longer string would draw short in the
  twin AND have a box too narrow for the pixel fallback to cover. It predates this landing on
  both sides and nothing here changes it; it is named because the note this landing adds to the
  engine map originally attributed our 256 to the engine, which would have hidden it. No fixture
  draws one: TA's HUD strings are short.
* **THE WINDOW BETWEEN THE CHECK AND THE CONSUMER'S DRAW CANNOT BE CLOSED FROM THE PRODUCER, and
  is not.** The generation makes the "already sent" half of a block true as of the moment the op
  is committed; the render thread may still throw its glyph atlas away between that instant and
  the drain, and then that one string draws with the missing characters dropped. It is bounded by
  a window, self-heals on the next publish, and `miss=` counts it — 0 over 19 585 string ops here.
  **The reviewer's proposed tightening was rejected after verification**: raising `g_guiq.reseed`
  when `twin_string` records a miss would make the losing window the last one, but `miss` is not a
  divergence signal — a string containing any code the font has no glyph for increments it while
  the engine skips that code too, which `tagpu_gui_surf.c`'s own comment says at the counter. That
  would re-seed every surface on an ordinary string.
* **A font whose header lies is refused exactly as before and no better** — `f[3] != 0`,
  `f[0] == 0` and `gfont_glyph`'s zero tests. A table entry pointing outside the loaded file
  image, or a width byte that runs the bits past its end, is read by us and then by the engine one
  instruction later; this landing neither adds nor removes that.
* **The level gate that was removed had never fired on any measured fixture** (`strstale=0` over
  the control's whole walk), because every string is in game where `level_tracked()` is 1. So the
  change is established to move no pixel, and its value is the lifetime argument rather than a
  behaviour it corrects.
* **`tagpu_packet_pub_level_tracked` and `tagpu_reclaim_level_closing` now have no caller.** They
  are kept as those modules' level API; deleting them is a separate decision.

### The gate's walk — the Vulkan lane against the GL lane, over the whole inventory

`tools/uiwalk.py --vk`.

**WHAT THE GATE ASKED FOR IS NOT WHAT WAS RUN, and the substitution is the first thing to say.**
The gate's wording is a **`strict`** walk over the full screen inventory at both resolutions, shell
and in game, with the Vulkan lane matching what the GL lane scores. `strict` is `uiwalk`'s existing
mode and it means something specific: it diffs **our** frame against the **engine's own surface**
and counts the holes. That is a parity oracle against the engine, and **it is not a valid
regression with Classic++ on** (see above — Classic++ is deliberately not the engine's output), so
it cannot answer a question about the Vulkan lane at all. `uiwalk` also had no Vulkan support
whatever before this landing.

What was run instead is an **A/B of the two lanes against each other**: same process, same frame,
same UI ops, GL writes one capture and Vulkan writes the other, and the two are diffed pixel for
pixel. It answers *"does the Vulkan layer put the same pixels on the screen as the GL layer"*,
which is the question the gate is about. It does **not** answer *"are those pixels right"* — that
is `strict`'s question and both lanes could be wrong together and still score 0. The GL layer's own
parity against the engine is G15's evidence (§2.3e), measured under `norestore` where the oracle is
valid, and this walk inherits it rather than re-establishing it.

`--vk` arms the lane in its own window — `gui.on=mmbase classicpp.on vk.on=color=0,0,0`, **no
`norestore`**, so landing 4's restored UI atlas is live and is what the comparison runs through —
and at every stop re-arms `tagpu_gui.ab`, waits for both lanes to write, and diffs the pair with
`tools/vk-ab.py --pass gui`. The lever is one-shot per arming (`s_abDone`), so it is created and
removed at every stop rather than left standing; it is removed **after** `vk-ab.py` has run, not
before, because `vk-ab.py`'s own "these captures predate the lever" staleness check only runs while
the lever is still on disk.

| walk | stops at 0 px | non-black px a side, min–max | of |
|---|---|---|---|
| shell, 640×480 (what the shell runs at whatever the game res) | **13 / 13** | 297 477 – 307 200 | 307 200 |
| in game, 640×480 | **13 / 13** | 93 688 – 145 437 | 307 200 |
| in game, 1024×768 | **13 / 13** | 118 232 – 174 781 | 786 432 |
| in game, 1920×1080 | **13 / 13** | 175 576 – 232 125 | 2 073 600 |

**52 of 52**, and **the ink column is half the claim.** On every one of the 52 rows the GL lane's
non-black count and the Vulkan lane's are the **same integer** — not merely both non-zero — so each
0 px is a diff over a frame that had content, and had the same amount of it on both lanes. A row
with 0 px and 0 ink is two blank frames agreeing and is refused, not counted; that is the fourth
guard below, and it is the reason these figures are a re-measurement.

The in-game walks include the four-deep stack `VISUALRT` over `PREFS` over `ARMOPT`
over `ARMCOM1` over `ARMMAIN2`, both pages of the build menu, chat and F4; the shell walk includes
`SELMAP` over `SKIRMISH` and our own injected `VISUALS.GUI` with its 50 gadgets.

**It was 39 until the run that produced these figures, and the extra 13 are the 640×480 walk's
in-game half.** The earlier set took the 640×480 row from a shell-only run, so the resolution the
shell actually renders at had no in-game stops at all; this set walks both halves there. The other
two rows are unchanged — the same stops, the same ink, to the byte — which is also what says the
landing that prompted the re-run moved no pixel.

The in-game ink is a **smaller fraction** of the frame than the shell's because the shell is UI
edge to edge while in game the UI is the panel, the bars and the strings over a world the A/B
blacks — 31–47 % of a 640×480 frame, 15–22 % of a 1024×768 one, 8–11 % of a 1080p one. The
fraction falling as the frame grows is the expected shape: the panel is a fixed pixel size and the
world around it is not. It is the count of pixels **this layer** put down, which is what the
comparison is about.

**A STOP WITH NO USABLE COMPARISON IS NOT A ZERO, and this is the part of the walker that matters
most.** `vk_ab` returns `None`; the stop prints `NO COMPARISON` and the report renders it **NO
COMPARISON** and counts it *out* of the pass tally. Four runs earned four separate guards, every
one of them found by a walk that had already reported a pass:

* **Neither lane wrote.** The first walk ran against an instance a killed run had left part-driven,
  and the game exited a third of the way through. Twelve stops had no pair at all — against a dead
  game. Rendered as `0 px` they would have read as twelve passes, and the walk would have claimed
  17 of 17.
* **A capture caught mid-write.** The first 1080p walk read `tagpu_gui_vk.ppm` at **5 509 120 of
  6 220 800 bytes**, because "both files exist, sleep 0.4 s" is enough at 640×480 and not at 1080p.
  It now polls until each file reports the same size twice running.
* **The settle compared against the wrong number.** That poll then checked the settled size against
  `--res`, i.e. the *game's* resolution — but **the shell runs at 640×480 whatever the game
  resolution is**, so a 1024×768 shell walk waited for 2 359 296 bytes against a real 921 615, never
  settled, and fell through on the 40 s deadline at **all 13 stops**. Every one of them was taken by
  the timeout: by exactly the "both exist, then hope" behaviour the poll had been added to replace.
  The expected size now comes from the **PPM header**, so it is the frame's own resolution.
* **Both captures blank.** `vk-ab.py` prints `differing px 0 of N` *first* and only then decides
  that `diff == 0 and ink_gl == 0` means "BOTH CAPTURES ARE BLANK — that is not a pass" and exits 1.
  The walker read the count and never the exit status, so a stop where the layer composited nothing
  scored **0 px and rendered as a pass**. The A/B blacks the frame and the layer's shader `discard`s
  every fragment it does not own, so two all-black captures agree perfectly and prove nothing. The
  exit status and the `non-black px` line are both read now, and **the ink is a column in the
  report** — a 0 px row is only a pass with a non-zero ink beside it.

* **The walk ran even when there was nothing to walk.** `tacli launch` and, worse, `tacli scenario
  load` both had their exit status captured into `rc` and never looked at. A failed load sent the
  walk into `game_walk` against whatever was on screen — the shell, most likely — where the stops
  would find real content, diff it, and report 0 px with a healthy ink count. Every guard above is
  about telling a hole from a pass at one stop; this was a hole upstream of all of them, and none
  of them could see it. Both calls now pass `check=True` and raise with tacli's own output.
  [FOUND 2026-09-16, reconciling why one walk had recorded no in-game stops at all.]

The middle two were found by a review of the walker *after* it had produced a "39 of 39", which is why
**all three walks were re-run from scratch** under the corrected guards and the numbers below are
the second set. The first set is withdrawn: two of its stops' guards were weaker than the prose
describing them.

This is the failure mode this lane produces over and over — landing 1 stood down on every frame
while every counter read zero, landing 4 measured 0 px on a run that had restored nothing — and the
walk is the one place where a hole and a pass look identical unless the tool refuses to conflate
them. **0 px over content that is not there is not a measurement.**

#### Not covered by the walk

* **IT IS NOT THE `strict` WALK THE GATE'S WORDING ASKS FOR.** It is a lane-against-lane A/B, for
  the reason given at the top of this section. 0 px means the two lanes agree, not that either is
  right; the GL lane's own correctness is G15's, measured elsewhere and inherited here.
* **The blank-pair guard is a refusal, not coverage.** It can tell a stop where nothing was
  composited from a stop where the two lanes agreed on real content. It cannot tell a stop where
  *most* of the content was missing from both lanes: the ink column would be non-zero and the
  diff would be 0. A partial hole common to both lanes still scores as a pass.
* **The size-settle guard never engaged on the shell walk.** The shell runs at 640×480, where the
  captures are 921 615 bytes and the writes have always completed inside one 0.4 s poll; the
  mid-write case was only ever observed at 1080p. So the shell walk's 13 stops exercise the
  *settle-on-equality* path but never the *wait* it exists for, and the guard is evidenced by the
  in-game walks alone.
* **The inventory is the screens, not every state of them.** 13 stops per walk. A build menu page
  the walk does not turn, a dialog it does not open, an animation mid-frame: not covered.
* **One map, one side (ARM), one scenario** — `tascene-parity`.
* **The cursor and the minimap are at their landing-3 levers** (`mmbase`), not swept.

### 2.35 What the Vulkan lane draws in the SHIPPED configuration — measured, and it is the UI alone

**MEASURED 2026-09-16**, DLL `644ce2e`, instance launched with `--defaults` (the player's
configuration, not a bench one), `one-unit` on Two Continents at 1024x768, the reference setup's
4070, `tagpu_vk.on` armed live. This is landing 1 of [vulkan-only-plan](vulkan-only-plan.html) and
it writes no code: the whole deliverable is this section.

**Every Phase G figure was taken under `tagpu_defaults.off` + `ss=1` + `gui.on=mmbase`.** Nobody
had started the lane in the configuration the patch ships in. The `opt:` line for this run is the
shipped one —

    opt: play defaults ON (no tagpu_defaults.off): native=all wrecks owndraw=all terr terrown
    feat featown fx sfx fxown mark markown order ghost zoom vpwide gui classicpp weapons

— all eighteen passes `ARMED`, `ghost: ARMED alpha=0.40`, no `tagpu_ss.off` so supersampling is
at its shipped `2x`.

**The result: the Vulkan window presents the UI and nothing else.** Of 786 432 px at 1024x768,
**630 589 are the lane's clear colour** — the whole viewport. The side panel, the top and bottom
bars, the minimap and the resource readouts are all there and correct; the world is not drawn at
all. The lane is not broken and nothing crashed: every world pass **stood down on purpose**,
which is the behaviour each of them documents.

#### The two causes, and they are independent

**1. The Classic++ restored atlases have no CPU mirror.** Four passes log the same refusal, once
each, and then draw nothing:

    vk: unit: the GL twin is drawing through the Classic++ restored atlas and that surface has no
        CPU mirror - the Vulkan edition draws nothing this session rather than draw a different
        picture from its own oracle
    vk: terr: ... the Classic++ restored tile atlas ...
    vk: feat: ... the Classic++ restored atlas ...
    vk: fx:   ... the Classic++ restored atlas ...

`classicpp` is a **play default**. So in the shipped build this refusal is the normal case, not
an edge one. The GAF and tile atlases already have opt-in CPU mirrors for the indexed path
(`tagpu_gaf.c`, `tagpu_terr.c`'s `s_mirrorWant`); the *restored* surfaces the restorer paints do
not, and a Vulkan pass cannot read a GL texture.

**2. The cast-shadow map has casters this lane cannot draw**, and it takes two more passes with
it:

    vk: shadow: the GL map holds 1 caster(s) this lane has no copy of (0 of them the unit pass
        carries) - the native 3DO stream and the replacement meshes are still to port. Nothing
        drawn while there are, and the passes that sample the map stand down with it
    vk: unit: the GL twin drew these units against a cast-shadow map and the Vulkan lane has none
        this frame - nothing drawn while that is true
    vk: terr: the GL twin is reading the Classic++ cast-shadow map and this frame's Vulkan map was
        not drawn (its casters are not all on this side of the seam yet) - nothing drawn rather
        than a different picture from our own oracle

**Cause 2 alone is enough to blank the world.** With `classicpp.off` armed live the FEATURES and
their shadow splats appear — trees, rocks and their shadows over the clear colour — and the
terrain and the commander still do not. So turning Classic++ off buys the feature pass and
nothing else.

#### The stand-downs are SESSION-LATCHED, and the word in the log is literal

*"draws nothing **this session**"*. Each refusal sets a `s_said*`-style latch and the pass does
not come back when the condition clears: after `classicpp.off` and then `classicpp.off=off`, the
feature pass that had been drawing was dark again and stayed dark. A player who toggles Classic++
mid-game does not get the world back; only a relaunch does. This is worth stating because it
makes the lane's behaviour under a *live* lever different from its behaviour at launch, and every
Phase G measurement armed its levers before the lane came up.

#### What this does to the plan's landing order

[vulkan-only-plan](vulkan-only-plan.html) had `tagpu_vk_mark.c` at landing 3 and the restorer at
landing 5. That order is wrong: **a restored-atlas CPU mirror and the 3DO / replacement-mesh
caster stream are prerequisites for any world pixel at all** in the shipped configuration, and
the mark pass draws over a world that is not there. They move to the front.

**The build ghost is moot for now.** `tagpu_vk_unit.c:1316`'s `otherDraws` stand-down — the one
a build ghost or a frame past `TAGPU_PD_MAXHAND`'s 512 units trips — was never reached, because
the unit pass refuses on the atlas mirror several checks earlier. It stays on the list; it is not
the first thing in the way.

#### Not covered

* **One map, one scenario, one resolution, one GPU, one OS.** Two Continents, `one-unit`,
  1024x768, the reference setup's 4070 under Wine. S3 has still never run.
* **The shell was not walked** in this configuration — only in game.
* **`ss=2` was on and is therefore untested as a difference**: with no world drawn there is
  nothing for the supersample factor to change. The gap named in the plan (the lane has no
  offscreen world target) is unaffected and still unmeasured.
* **`tagpu_gui.off` was not tried** here; the plan's reading of `tagpu_vk_gui.h` stands
  unmeasured.
* **No pixel A/B was run.** This section is an inventory of what draws, not a parity figure.

### 2.36 The Classic++ restored atlases, mirrored for Vulkan — gate 2 of the Vulkan-only plan

**MEASURED 2026-09-16.** [gpu-status](gpu-status.html) §2.35 found that in the shipped
configuration the Vulkan lane drew the UI and nothing else, because four world passes each stood
down on the same refusal: *the GL twin is drawing through the Classic++ restored atlas and that
surface has no CPU mirror*. This closes that for three of them — features, effects and terrain.
The unit pass is not here; see *Not covered*.

**A Vulkan pass cannot read a GL texture**, which is why the refusals existed at all. The fix is
the mechanism `tagpu_gaf.c` already had for the UI atlas — `tagpu_gaf_atlas_mirror_rgb` and
`_rgb_step`, whose only caller was `tagpu_gui_surf.c:2476` — extended to the world atlases.

#### What each pass needed

**Features and effects** are `TAGPU_GAFATLAS` instances, so they ask for the same mirror on the
same arm beat as their indexed one, behind the same latch and gated additionally on
`tagpu_classicpp_assets()` so a session with Classic++ off never pays the second 16 MB. One
read-back step per published frame, in the publish rather than the arm beat, because it is a
`glReadPixels` off an FBO and that is the render thread with the context current.

**Terrain needed a different mechanism.** Its existing mirrors are *"the buffer the
glTexImage2D was handed, kept instead of freed"*, which cannot work for `s_rgbTex` — the
restorer paints that on the GPU. And `tagpu_terr.c` had none of the entry points, because
`glReadPixels` is not among the fork's own globals and has to be fetched. So
**`tagpu_gl_rgba_readback()`** was added to `tagpu_gaf.c`, where those entry points are already
resolved: a caller-owned FBO made once, the attachment dropped on every path so a caller's
texture is not kept alive past a delete, `GL_PACK_ALIGNMENT` and the bound framebuffer restored.
It knows nothing about an atlas.

**The content key is the part that decides whether this works at all.** `tagpu_gaf.c`'s own step
records why, from a landing it cost two rounds: *a serial that is not the CONTENT's serial
uploads once and then misses everything after it.* Terrain keys on the restorer's paint count
**plus** the row count, because `ensure_atlas` can grow the atlas without a paint landing in
between.

#### Three ordering faults, all found by measurement

The terrain half did not work when first written, and the A/B is what said so. Each fault was
hidden behind the one before it, and all three are the same shape — **a refusal placed above the
code that would satisfy it**:

1. The read-back was gated on `s_job`, which `tagpu_terr.c:1109` frees the instant the restore
   COMPLETES (*"the texture is ours"*). It therefore zeroed the mirror at exactly the moment the
   twin became fully painted. The twin is `s_rgbTex` plus `s_rgbState` (1 restoring, 2 complete);
   the job is only meaningful during state 1, and the key takes a `-1` sentinel once there is no
   counter left to read.
2. The restored image's **resize** sat after the refusal that tests it. `prepare` returning 0
   skips everything below, so the image was never created — measured as `img=0 view=0` against a
   perfect hand-over (`rows=2720 serial=1`).
3. The **upload** sat after the refusal too. Image created, `have=0`, the same deadlock one level
   down.

The refusal now sits after the uploads, so the data lands and the test passes in the same frame.

**AND THE REFUSAL TESTS THE VIEW, NOT THE HAND-OVER.** A first version tested `t.atlasRgb`,
which meant a resize failure left binding 42 naming the **indexed R8** image while the refusal
passed — the shader would have sampled R8 through an RGBA sampler and drawn a wrong picture
instead of standing down. Testing the view and its contents makes a failure self-correcting: the
binding falls back to the indexed view, which keeps the descriptor valid, and the branch goes
unreachable again. The placeholder comments the G19e author left at each of these bindings
(`tagpu_vk_feat.c:773`, `tagpu_vk_fx.c`'s 43, `tagpu_vk_terr.c`'s 42, and `tagpu_vk_unit.c`'s 43
which is still one) say *"this is the binding that stops being a placeholder"*; three of them now
have.

**The refusals are also no longer latched.** §2.35 measured what *"draws nothing this session"*
costs: a pass that refuses once stays dark for the process even after the condition clears. These
conditions clear by themselves as the restorer paints, so the flag now gates the log line and not
the refusal.

#### What was measured

One-unit and fx-lasers on Two Continents, 1024x768, `ss=1`, one pass armed at a time, the
reference setup's 4070:

| pass | fixture | result |
|---|---|---|
| features | feat-forest | **0 px of 786 432**, 243 695 non-black a side |
| effects | fx-lasers | **0 px of 786 432**, 2 557 non-black a side (3 452 on the first run: the duel's ink moves with what is in flight) |
| terrain, Classic++ **off** | one-unit | **0 px of 786 432**, 630 606 non-black a side |
| terrain, Classic++ **on** | one-unit | **5 px of 786 432**, worst channel 1, 630 774 non-black a side |

**Every row above was taken twice: once before the landing review and once after its fixes**, and
the two agree — the same counts, and the terrain 5 at the same pixel with the same two values. The
figures quoted are the post-fix ones. That is the point of re-measuring rather than carrying the
first numbers forward: three of the five findings changed code on the path these captures go
through.

**The terrain 5 px are not a regression, and the previous-build A/B is what established that.**
On `bfbe8b6`'s DLL the same fixture gives **0 px with Classic++ off** and **no picture at all**
with it on — the pass stood down. So the indexed path is unchanged and the 5 px belong to the
newly-enabled restored path, which previously produced nothing to compare against. They are
deterministic: three runs, same count, same first pixel (721, 173), GL (60, 60, 60) against
Vulkan (59, 59, 59). That rules out a stale or racing mirror, and the magnitude rules the mirror
out anyway — a wrong read-back differs in thousands of pixels, not five. One level on all three
channels, arising only when Classic++ turns `uRestored` and `uLit` on together, is float rounding
in the lit path. 0.0006 % of pixels at one level is inside the bar already accepted for restored
art ([renderers](renderers.html) 4c: max 1 level on 0.0012 % of bytes).

**The features 0 px covers less than it looks.** `restoreglsl: feat: lazy restore armed` and
nothing after it — the restorer had painted no feature frame in that fixture, so the twin was
alpha 0 throughout and both lanes fell back to the palette per texel. It proves the upload
corrupts nothing, that the `uRestored == 1` branch is reachable on both sides, and that the
fallback through it is identical. It does not prove restored feature colours match.

#### The landing review, and the two faults no picture would have shown

Five findings at `medium` on the accumulated diff, all five verified against the code and acted
on. **Two of them were invisible to every measurement above** — the pixels were already 0 px and
the build was already clean — which is the case this gate is worth writing down:

1. **A heap overflow in the terrain mirror's allocation, CONFIRMED.** The growth test read
   `s_rgbMirrorRows > s_atlasH` — the rows last *read* against the atlas's height — so it fired
   only when the atlas SHRANK, and the zero path sets those rows to 0 on the very map change that
   precedes a growth, so it could never fire at all. The buffer then stayed at the first map's
   size while `rows = s_atlasH` grew with the second and `glReadPixels` wrote past the end: a
   1000-tile map followed by Two Continents is **4.7 MB allocated and 23.7 MB written**. The test
   is now the ALLOCATED height (`s_rgbMirrorCap`), and the new buffer is allocated before the old
   one is freed so a refused `calloc` leaves a working mirror rather than none.
2. **A 16 MB-per-frame re-upload, for ever, CONFIRMED.** `doRgb` compared a stored row count
   against the hand-over's, but what was stored was the rows *sent* — forced up to `atlasDim` —
   which can never equal the shelf-bounded rows the producer publishes. So the comparison was
   always unequal, the upload ran every frame on both the feature and the effects pass (**~32 MB
   a frame between them**), the staging buffer was never given back, and a `mk_buffer` failure
   would eventually take the pass down for the session. The serial alone decides now; the field
   was renamed (`s_arReq`) because a name that says "rows" while holding "rows I asked for" is
   how this happened.
3. **An image lost with its memory still bound, CONFIRMED.** A partial `mk_image` failure nulled
   the image and view but left the allocation, so the next `kill_image` would `vkFreeMemory`
   memory with a live image bound to it. `kill_image` on that path, which is what
   `tagpu_vk_terr.c`'s `shared_resize` already did for the identical case.
4. **The restored resize ignored its return value, and ran before the bounds.** `shared_resize`
   returns 0 when a retire is still outstanding — one at a time, by design — and the call site
   dropped that, leaving the PREVIOUS image standing with `view && have` set. The refusal then
   passed and the upload memcpy'd `s_rgbAtlas.w * s_rgbAtlas.h * 4` bytes out of a mirror holding
   `atlasRgbRows`: a read past the mirror when the deferred size was the larger, the wrong rows
   sampled when it was not. Two map changes inside one turn of the slots is what it takes. It now
   sits with the other two shared images, after the bounds block, with its return read the same
   way; `t.atlasRgbRows` is bounded against `t.atlasH` in that block (the producer's bound is a
   bound only while both files are read together); and `doRgb` additionally requires the image's
   own `w`/`h` to match, because *"I asked for this size"* is not *"the image is this size"*.
5. **The helper duplicated the step it was extracted from.** `tagpu_gl_rgba_readback` re-stated
   `tagpu_gaf_atlas_mirror_rgb_step`'s body line for line — ~35 lines, a second place for the
   pack alignment, the saved binding and the dropped attachment to be got wrong. The step now
   CALLS it. That needed one addition to the helper: an optional `status` out-parameter carrying
   the `glCheckFramebufferStatus` value, because an **incomplete** framebuffer is a permanent
   property of the texture and both callers latch on it, which a 0/1 return cannot tell them.
   Terrain latches on it too now, and drops the mirror and its rows when it does — a mirror
   nothing will read again is up to 23 MB held, and leaving the ROWS standing would be worse than
   the memory, because the hand-over publishes on `rows > 0` and the consumer would go on drawing
   the last read-back while the restorer painted past it.

**And the prose was a finding of its own.** Each pass's *"WHAT IT DOES NOT DO"* header still
opened *"CLASSIC++'s RESTORED ATLAS IS NOT MIRRORED"* with the new paragraph stacked underneath
it, so a reader who stopped at the first sentence got the opposite of what the code does. The
leading claim is rewritten in each of the three files rather than contradicted, on the rule this
fork already applies to its counters (`gafstale` → `gafnoplane`): **the statement changes with the
meaning.** `tagpu_vk_unit.c`'s copy is left alone and is still true.

One statement in the code was wrong in the other direction and is also corrected: the terrain
refusal's comment claimed the frame that uploads is lost and the next one draws. It is not —
`shared_upload` sets `have` as it RECORDS the copy, into this frame's own command buffer ahead of
this frame's draw with a barrier between them, so the upload and the draw are the same frame. The
log bears it out: **zero refusal lines** in the post-fix runs, on a fixture whose twin restores to
completion.

#### Not covered

* **The unit pass.** Its mirror is not in this landing: it cannot be verified until the caster
  stream lands, because `tagpu_vk_unit.c` stands down on the cast-shadow map first — *"the native
  3DO stream and the replacement meshes are still to port"* — so an unmeasured consumer would be
  an unfinished unit of work. `tagpu_vk_unit.c`'s binding 43 stays a placeholder.
* **Restored FEATURE and EFFECT colours**, for the reason above: the lazy queues had painted
  nothing in either fixture. Terrain is the only pass here whose restore runs to completion.
* **One map, one resolution, one GPU, one OS**, and `ss=1` — which the A/B requires and the patch
  does not ship.
* **The 5 px are attributed, not traced.** The argument is from magnitude and from which uniforms
  turn on together, not from a line of shader arithmetic.
* **Findings 1, 2 and 4 are fixed but not reproduced.** Each needs a map change (two of them in
  quick succession, for 4), and the A/B fixtures are single-map by construction; the arguments are
  from reading the code against the allocation and the serial. An in-process map change brings the
  whole Vulkan lane down and back up, so the two-map test is a fresh lane rather than a resized
  one and does not exercise the retire path the fourth finding is about.

### 2.37 The caster census, measured before it was ported — gate 3a of the Vulkan-only plan

**MEASURED 2026-09-16.** The Vulkan-only plan filed landing 3 as *"the caster stream — the native
3DO stream and the replacement meshes, so the cast-shadow map can be drawn on this side of the
seam and the passes that sample it stop standing down."* The exit condition was right and the
mechanism named in it was wrong, in both directions: one of the two things it named cannot
happen at all, and the thing that was actually blocking every world pass was not a caster.

#### What the census actually holds

`tagpu_shadow.c` counts, as `otherCasters`, every caster it drew into the GL map that the Vulkan
hand-over carries no copy of. Four kinds were named. Reading the code against the numbers:

| caster kind | status |
|---|---|
| the posed bodies | **covered** since G19e — `tagpu_vk_unit_cast`, subtracted as `tagpu_vk_unit_casters()` |
| the native 3DO stream | **cannot occur** — dead code, below |
| the replacement meshes | **live and uncovered** — gate 3b |
| the heightfield with no mirror | uncovered by design; an out-of-memory path both G19e reviewers found |

**THE NATIVE 3DO STREAM IS DEAD CODE.** `tagpu_shadow_unit` has exactly one call site
(`tagpu_native.c`, the caster loop) and it sits behind `if (skip || firstv[i + 1] == firstv[i])
continue;`. `nv` is 0 at the top of that unit loop and is not incremented anywhere inside it —
the first increment is `emit_fx_model`, *after* `firstv[nu] = nv`. So `firstv[i+1] == firstv[i]`
for every unit, the caster draw never runs, the body `glDrawArrays` beside it draws zero
vertices, and `otherCasters` never counts a native-stream caster. The file says as much in its
own words three hundred lines earlier — *"an ordinary unit contributes no vertices either now"* —
since G16 step 8 made the posed program the path. **There is nothing to port.** The code is
marked where it stands rather than deleted; deletion is the plan's landing 11.

#### What was actually blocking every world pass

With the restored atlas **off** and four posed units on screen under Classic++ soft shadows, the
census closes on its own: `otherCasters - ours == 0`, no refusal on any pass, the shadow pass
uploads its caster mesh and draws, and the terrain and unit passes both draw into the Vulkan
frame. With the restored atlas **on**, the same fixture: *"the GL map holds 1 caster(s) this lane
has no copy of (0 of them the unit pass carries)"*.

The difference is not a caster. The unit pass stood down on **the Classic++ restored atlas**
several checks before it reached its casters, so `tagpu_vk_unit_casters()` answered 0 whatever
the casters were, and the census refused every frame with a unit on it — which stood the shadow
map down, which stood the terrain down. §2.35's *"the lane draws the UI and nothing else"*, read
from the other end: **one missing mirror, propagating through three passes.**

So gate 3a is the half §2.36 deferred — the unit atlas's restored twin — and the caster stream
proper is gate 3b.

#### The mirror, a fourth time

Gate 2's mechanism applied unchanged: `tagpu_render3do.c` gains `_mirror_rgb_want` / `_step` /
`_mirror_rgb` beside the indexed trio, asked behind `tagpu_classicpp_assets()` so a session with
Classic++ off never pays the second 16 MB and stepped once per published frame on the render
thread; `tagpu_posedraw.c` publishes `atlasRgb` with **its own rows and its own serial**; and
`tagpu_vk_unit.c` takes a second RGBA8 image at binding 43, which stops being the placeholder the
G19e author left there.

**Gate 2's five findings were applied here before they could be made again** — the serial ALONE
decides the upload, the rows are bounded against the atlas's own square in the consumer, the image
is built before the refusal that tests it, and a device refusal of the image is non-fatal. The
refusal also stopped being a statement about the session: it said *"draws nothing this session"*
and now says *"until the read-back produces rows"*, which is a few frames.

#### THE MEASUREMENT WAS WRONG FIRST, AND EVERYTHING ELSE FOLLOWS FROM THAT

The first version of this landing measured **0 px** and was wrong in four independent ways at
once. The reason it could be is one line in `tagpu_native.c`:

```c
if (fxOn || sfxOn || featOn || terrOn || markOn) {
    …
    tagpu_rglsl_step();          /* the restorer paints HERE, and only here */
}
```

**A unit A/B armed with `native.on` alone never steps the restorer.** The twin is created, the
job is armed, `restoreglsl: unit: lazy restore armed` appears in the log — and nothing is ever
painted. The `uRestored == 1` branch then samples alpha 0 on both lanes and both fall back to the
palette per texel, so the two agree perfectly *about a branch neither of them exercised*.

**`mark.on` is the lever that fixes it**: it is on that list and it is the only one with no
Vulkan pass of its own, so the restorer paints and the unit pass is still the only pass drawing
into the Vulkan frame — which the capture guard requires. With that one file added, the same
fixture went from **0 px** to **2 126 of 2 132 unit pixels differing**.

**The check that the twin actually painted costs nothing and there is no log line for it.**
`tagpu_restoredump.on` writes the twin once the job goes idle; failing that, run the fixture twice
with `assets=1` and `assets=0` and `cmp` the two **GL** captures. Byte-identical means the
restorer painted nothing and the run says nothing about restored art. §2.36's feature and effects
figures carry exactly this caveat, and now it is known why.

#### Four filter faults, each hidden behind the one before

With the fixture honest, the residual came apart in order. Every figure is `selbox-facings` on
Two Continents, `ss=1`, the unit pass's own A/B (`tagpu_posedraw.ab`):

| what was wrong | why | worst channel |
|---|---|---|
| the image was `dim × rows` | the UVs are normalised against the whole atlas (`tagpu_gaf.c`: `u0 = x / a->dim`) and the GL twin is `dim × dim`, with no scale uniform between them — so `v = 1.0` meant row `rows`, off by `dim/rows` | — |
| binding 43 had the **indexed** sampler | the restored twin is the only texture this shader reads that holds true colour instead of palette indices, so it is the only one GL filters (`GL_LINEAR`) | 155 → 124 |
| the image had **one mip level** | `tagpu_gaf.c` gives a mipped twin `GL_LINEAR_MIPMAP_LINEAR` to `GL_TEXTURE_MAX_LEVEL`; a 32-texel cell lands on a ~23 px sprite, so LOD ≈ 0.5 and GL blends levels 0 and 1 in ordinary play | 124 → 122 |
| the mirror's levels 1+ were **one paint batch stale** | `twin_mips` runs at the top of a frame, `tagpu_rglsl_step` paints in the middle, the read-back runs at the end — so the read-back that first sees a new paint count sees level 0 fresh and the levels above it as they were *before*, then latches `mirroredPainted` and never looks again | 122 → **7** |

**The mip levels are read back, not re-derived.** A blit chain on the Vulkan side would be this
fork guessing at `glGenerateMipmap`'s reduction, and the guess would be a per-driver difference no
note could pin down. Reading GL's own levels makes the two byte-identical by construction — the
rule the whole seam runs on.

**The fourth one was found by looking at *where* the residual was rather than trying another
filter.** 422 of 581 differing pixels were more than 8 levels apart and the Vulkan side showed
flat greys where GL had colour: the signature of sampling a mip built from different texels, not
of a filter setting. Three filter guesses had moved the number by 33 levels between them; reading
the difference image moved it by 115.

#### Anisotropy: the one difference that is not a bug, and the owner's call

With everything above carried across, the residual was **566 of 2 132 unit pixels at 1024×768 and
413 of 1 528 at 640×480, worst channel 9**, ink identical. Setting the twin's anisotropy to 1 on
**both** lanes gives **0 px of 786 432 at 1024×768 and 0 px of 307 200 at 640×480**.

So the whole remainder is the anisotropic filter. Both APIs leave sample placement to the
implementation and the same driver does it differently for each — there is no Vulkan sampler
state that reproduces GL's, and none is coming.

**The owner's decision (2026-09-16): play keeps 4× on both lanes; the A/B is taken at `aniso=1`
as a stated substitution.** It is the same shape as G19f's walk, which compared the lanes rather
than running `strict`: the oracle still catches every porting mistake and excludes only the one
thing the specs leave free. `aniso=` is a `tagpu_classicpp.cfg` knob, default 4, and **both lanes
read it** — `tagpu_gaf.c` applies it and publishes what it actually got, `tagpu_vk_unit.c` builds
its sampler from the same number, and a knob changed mid-session makes the two disagree, which is
caught: a sampler cannot be rebuilt mid-frame for the same reason a shared image cannot, so the
pass compares the published ratio against its own and stands the frame down.

`samplerAnisotropy` is enabled on the device for this, exposed as `anisook`/`maxAniso` beside
`flipok` and `zclipok` — the first core feature bit the seam asks for rather than an extension.

#### What was measured

| configuration | result |
|---|---|
| pre-gate-3 DLL, Classic++ art on | **no picture at all** — the pass refused for the session |
| `aniso=1`, 1024×768 | **0 px of 786 432**, 2 128 non-black a side |
| `aniso=1`, 640×480 | **0 px of 307 200**, 1 525 non-black a side |
| the 4× play default, 1024×768 | the pass **draws**: 566 of 2 132 unit px, worst channel 9, ink identical |

The first row is what this landing changed: the same fixture drew *nothing* before it.

**AND ALL THREE ROWS WERE RE-TAKEN AFTER THE RE-REVIEW'S FIXES, to the same numbers** — 2 128 and
1 525 non-black a side, 0 and 0, and the same 566 at worst channel 9. That re-run is not a
formality here: the `s_arReq` fix makes the partial-row upload REACHABLE for the first time, so
every figure above it had been taken with the whole square going up every time. The picture is
the same either way, which is what it should be, and now it is measured that way rather than
assumed. The short-chain refusal is evidenced from the other side by the same run: the Vulkan
column is 2 128 non-black, not 0, so the refusal did not fire on a healthy session.

#### What the RE-REVIEW found, and the one it found in this page itself

Six commits went to a dedicated reviewer after the fixes above, because the fixes were larger
than the landing they were correcting. Nothing was wrong in the part that was expected to be:
all 25 `mk_image` / `img_barrier` / `copy_rect` call sites carry the right level counts, the
`samplerAnisotropy` feature survives every rung of the device retry ladder, and the `mippedN`
content key converges after exactly one extra read-back rather than spinning. What it found:

* **THIS PAGE CARRIED §2.35, §2.36 AND §2.37 TWICE, and one of the two §2.37s was the draft the
  fixture discovery had already invalidated.** The docs commit was a character-offset splice that
  re-appended 1 298 lines instead of replacing 108 — visible in `git diff` as an insertion with
  **zero deletions**, which is impossible for a section that was rewritten. A reader hit the stale
  figures first. **This is the third time this exact thing has happened on this page** (§2.34's
  own text records the other two), and the check that catches it every time is the one this
  landing skipped: regenerate the wiki and read the headings of the page you just wrote.
* **`s_arReq` held the rows SENT, so the partial-row upload was unreachable.** The whole-square
  rule forces the first send to the atlas's full height, so the count latched at `atlasDim` and
  every later frame's shelf-bounded count compared as a shrink: 21 MB memcpy'd per serial change
  — which is most frames while the restorer paints — and 21 MB of host-visible staging reserved
  per slot in a 32-bit address space. **This is gate 2's finding 2, one atlas down**, and
  `tagpu_vk_feat.c` stores the published rows for exactly that reason and says so in its own
  declaration. This pass's comment claimed the same discipline while the code did the opposite.
* **A SHORT MIP CHAIN WAS TO BE DRAWN RATHER THAN REFUSED, and it would have been drawn by
  destroying a live image mid-frame.** The producer handed over "the levels that were actually
  read", on the reasoning that the consumer should build a shallower image rather than one with
  an undefined level in it. Both halves are wrong: GL still filters the twin to its own
  `MAX_LEVEL`, so a shallower Vulkan chain is a *different picture* wherever a unit is minified,
  which is ordinary play — the same thing the anisotropy check beside it stands down for — and a
  depth that moves makes `atlas_rgb_build` `kill_image` an image the other slots' submitted
  command buffers still name, with no fence between. **That is gate 2's confirmed use-after-free
  in a second place.** The fix is at the producer: a chain that did not come back whole is
  reported as no mirror, the consumer already draws nothing without one, and because `mip` is the
  atlas's compile-time depth every publish that carries rows now carries the same depth — so the
  rebuild is unreachable by construction rather than rare.
* **`mk_image`'s failure label left the VIEW as it found it.** `vkCreateImageView`'s out-param is
  undefined on failure and every caller's `kill_image` destroys the view on the strength of it
  being non-NULL. `atlas_rgb_build` had a second `kill_image` to close its own call site; the
  other six were open. One line in the label closes all seven and retires the workaround.
* **A documentation claim about how to make the restorer paint was too absolute.** The skill note
  and the roadmap said `tagpu_rglsl_step` is called from `tagpu_native.c` inside
  `if (fxOn || sfxOn || featOn || terrOn || markOn)`, so that `native.on` alone paints nothing.
  There is a **second caller**: `tagpu_gui_surf.c:2465` steps the restorer whenever the UI atlas
  has a job of its own and nothing else stepped it this frame. The practical advice survives —
  see the corrected wording there — but it is advice from a fixture, not a guarantee from the
  code.


#### And a verification pass over the fixes, because this lane has a record

The four fixes went through a second, narrowly-scoped read — not another landing review, a check
that each fix does what it claims and introduces nothing — on the strength of this lane's own
history: a re-review has found a defect *inside* a fix three rounds running. All four verified.
What it returned instead was three comments asserting the pre-fix world, a guarantee that was not
where it looked, and one hazard:

* **`s_arReq`'s declaration still said "the rows LAST SENT"** — the exact wording the fix's own
  commit message pillories — and the producer's level loop still argued for the shallower image
  that finding 4 overturned. A comment left asserting the opposite of the code beside it is how
  the next reader inherits the bug.
* **The `mk_image` guarantee was a property of the CALLER, not of `mk_image`.** Its three *early*
  returns still left the view — and two of them the memory — as the caller found it, so "every
  out-param is NULL on this path" held only where a `kill_image` happened to run first. All four
  exits now null all three, which is the kind of guarantee a new call site inherits.
* **`mip` IS NOT IMMUTABLE, and the fix's stated invariant said it was.** `r3d_init` reassigns it
  on every GL context reset and the restore demotes it to 0 on a GL with no `glGenerateMipmap`.
  The fix survives on *ordering* — both writes land strictly before any publish carrying rows —
  but the argument had to be rewritten to rest on the invariant that actually holds. That is
  *Fixes must be safe by construction* applied to the argument rather than to the code.
* **AND THE SAME PAIR WAS A HEAP OVERRUN THIS LANDING HAD CREATED.** The mirror is deliberately
  never freed, so it outlives changes to the atlas it mirrors — and all three `memset`s plus the
  read-back sized themselves off the *current* `dim`/`mip`. A buffer allocated at `mip == 0`
  (16 MB) against a `mip` later raised back to 2 is a 21 MB write into it: ~5 MB past the end.
  **The mip chain is what made this expressible** — before this landing every one of those sites
  was the same constant `dim * dim * 4` — so it is this landing's hazard, not a pre-existing one.
  The buffer now records the shape it was allocated with, every write is bounded by that pair
  rather than by the live one, and the read-back refuses a frame whose atlas changed shape under
  it **without latching**, because the demote is re-applied at the top of the next restore and a
  latch would cost the mirror for ever over one frame. Reaching it needs a GL with no
  `glGenerateMipmap` and two context resets; the fix does not rest on that being rare.

The 1024×768 A/B was re-run after all of it: **0 px of 786 432, 2 128 non-black a side** — the new
refusal does not fire on a healthy session, which a guard this close to the hot path has to be
shown rather than argued.


#### Not covered

* **The replacement meshes — gate 3b, and not an optional case.** A tacli instance ships
  `hires/armpw.glb` **active** (`hires/off/` is a parking directory, not a lever), so the census
  refuses **1** caster with one Peewee on screen and **16** on the 257-unit `crowd-static`. Every
  world pass stands down on those frames.
* **The 4× difference is bounded but not traced.** 9 levels on ~27 % of unit pixels is what two
  fixtures at two resolutions showed; no attempt was made to derive it from either driver's
  sample pattern, and none is planned.
* **One map, one fixture, two resolutions, one GPU, one OS,** and `ss=1`, which the A/B requires
  and the patch does not ship. The 0 px is at `shadows=0`; with the cast-shadow map on, the
  soft-shadow PCF adds the 1 px §2.37 recorded before the fixture was fixed, and that pixel is on
  both builds.
* **The terrain and unit passes cannot be A/B'd in the same run** once shadows are on: terrain
  needs the unit pass drawing for the census to close, and two drawing passes make the lane refuse
  the capture. The chain closing is evidenced by the absence of every refusal plus the guard's own
  message naming two passes; the pixels are one pass at a time.
* **THE BANDWIDTH HALF OF THE `s_arReq` FIX IS ARGUED, NOT MEASURED.** What is measured is that
  the picture is unchanged with the partial path reachable. That the pass now sends the shelf's
  rows rather than the whole square on every serial change — 21 MB of memcpy and 21 MB of
  host-visible staging per slot — is read off the code and off `tagpu_vk_feat.c`, which has done
  it that way since gate 2. This pass logs no staging figure, and adding a counter to prove it
  would be instrumentation to leave behind.
* **THE SAME VIEW HAZARD IS STILL OPEN IN THE THREE SIBLING PASSES.** `tagpu_vk_feat.c:374`,
  `tagpu_vk_fx.c:344` and `tagpu_vk_terr.c:409` each `return 0` on a failed `vkCreateImageView`
  without nulling the out-param — and they leak the image and its memory besides, which the unit
  pass's label does not. It is the same one-line class fixed here and it was left alone
  deliberately: those files are outside this landing's diff, so no reviewer has read them in this
  landing, and widening a diff past what was reviewed is how an unreviewed change reaches `main`.
  Named here so it is a known debt rather than a silent one.

### 2.38 The replacement meshes' casters, drawn by Vulkan — gate 3b of the Vulkan-only plan

**MEASURED 2026-09-17.** Gate 3a left one caster kind uncovered and gate 3b is it. A tacli
instance ships `hires/armpw.glb` **active**, so one Peewee on screen made `tagpu_shadow.c`'s
census refuse **1** caster and a 257-unit crowd **16** — and a refused census stands the shadow
map down, and the terrain and unit passes down behind it.

#### What it is, and what it deliberately is not

`tagpu_vk_hires.c` draws those silhouettes into the cast-shadow map and **nothing else**. The
bodies stay with `tagpu_hires_draw.c`'s GL program: a glTF unit shaded per pixel with normal maps
is not what blocks the lane, the census is. That is the whole difference between this file and
its 1 400-line siblings.

Three parts, in the order they had to be built:

1. **`tagpu_hires.c` keeps the CPU vertex copy.** It freed `m->v` the moment the VBO had it —
   right while GL is the only consumer, wrong the moment a lane that cannot read a GL buffer
   needs the same triangles. The sizing written for this gate two days earlier said the vertices
   came "out of the static buffer"; **that was wrong and the code was checked rather than
   remembered.** The case worth the work is asking LATE: the lever can appear after the models
   are uploaded and freed, so the ask forces a re-read from file rather than answering NULL for
   the session.
2. **`TAGPU_HIHAND` records what `tagpu_hires_depth` drew, as it draws it.** `castSkip`, a VAO
   that would not build and a zero-count group each drop a unit from the GL map, and a second
   walk is free to disagree about any of them while both look right. It publishes only if EVERY
   caster the pass drew is in it: a record *short* of the GL map is worse than none, because the
   census counts the GL draws and a lighter map would satisfy it with fewer casters and come out
   **lit where the oracle has shadow**.
3. **The pass, and the census becomes a sum** — `tagpu_vk_unit_casters() +
   tagpu_vk_hires_casters()`. Both count the same way and both can only under-count, so the sum
   stays comparable to `otherCasters` by the same construction G19e established.

#### The albedo is deferred, and that is checked rather than hoped

The fragment shader's depth path is three lines: sample `uAlbedo`, `discard` if the alpha cutout
fails, return. One sampler read, one branch that can discard. The hand-over refuses any frame
carrying a group with `cutoff >= 0` (`cutoutSeen`), so that branch cannot be taken — which makes
the 1×1 white stand-in **not an approximation of the albedo but exactly equivalent to this path**.
The other six samplers the shader declares are untouched there and need a valid descriptor and
nothing more. Every material of the shipped `armpw.glb` is `alphaMode OPAQUE` (6 materials,
checked in the file), so the refusal has nothing to fire on today and is there for the mesh that
is not shipped yet.

#### What was measured

`hires-one` on Two Continents, 1024×768, `ss=1`, the unit pass's own A/B, `aniso=1`:

| configuration | result |
|---|---|
| before this landing, mesh active | **no picture at all** — the census refused every frame |
| mesh active, shadows on | **1 px of 786 432**, worst channel 1, 380 non-black a side |
| mesh **parked**, shadows on | **the same 1 px, at the same (603, 377), with the same two values** |
| mesh active, shadows **off** | **0 px of 786 432** |

The second and third rows together are the point: the pixel is there with **no replacement mesh
in the session at all**, so it is the soft-shadow PCF's and not this landing's. No refusal of any
kind appears in the log of the measured run.

#### AND THE SENSITIVE FIXTURE FOUND A DEFECT THAT IS NOT THIS GATE'S

`hires-one` draws 380 unit pixels whether shadows are on or off, so it barely tests a caster
silhouette at all. `crowd-static` — 257 units, 16 casters — draws ~220 000, and there the same
A/B is **not** clean:

| crowd-static, 1024×768 | differing | worst channel |
|---|---|---|
| mesh active, shadows on | 393 | 158 |
| mesh parked, shadows on | 435 | 158 |
| mesh active, shadows off | 267 | 158 |
| mesh parked, shadows on, `assets=0` | 164 | 176 |
| mesh active, shadows off, `assets=0` | **65** | 176 |

**Every row has the same first differing pixel, (285, 33).** It survives parking the mesh, turning
shadows off, and turning the Classic++ restored atlas off entirely — so it is neither gate 3b's
nor the restored path gate 3a built, though that path amplifies it from 65 px to 267.

**It is edge coverage, and that is measured rather than guessed.** Of the 65 pixels in the
cleanest row, **65 lie on a GL colour edge** — all of them — and in 26 the Vulkan value is
*exactly* a neighbouring GL pixel's. The differences are isolated singletons scattered from
x 201–870 and y 33–726, in both directions (Vulkan brighter in 27 of 65), and at (336, 69) GL has
blue (15, 39, 151) where Vulkan has grey (75, 75, 75) — two different UNITS winning one pixel.
Both lanes use `LESS`, so it is not a compare-op mismatch. What is left is the two rasterisers
disagreeing about which triangle owns a pixel whose centre an edge passes through: a 1-ULP
difference between two compilations of the same GLSL moves an edge across a sample point, and
with four units no edge lands on one while with 257 units 65 do.

**This is the same SHAPE as the anisotropy residual §2.37 escalated, and it is not yet the
owner's call because it is not yet traced.** It is 0.031 % of drawn pixels, bounded, symmetric,
and independent of everything this gate and the last one built — but **gate 3a's headline "0 px"
was taken on a four-unit fixture and could not have seen it**, and that is the honest correction
this section makes to the one above it. What it needs is its own landing: the cause is attributed
from the difference image, not traced to a line of arithmetic.

#### Not covered

* **The caster silhouette itself is not verified pixel-for-pixel.** The gate's exit condition —
  the census closes and the passes stop standing down — is met and measured. But the A/B that
  could see a *wrong* silhouette is the terrain pass's, and terrain cannot be A/B'd while the
  unit pass draws, because the census needs the unit pass drawing and two drawing passes make the
  lane refuse the capture (§2.37 records the same limit). The evidence here is the census closing
  plus the unit pass matching; it is not a picture of the shadow.
* **The edge-coverage defect above is characterised, not fixed, and not traced.**
* **One map, one resolution, two fixtures, one GPU, one OS,** and `ss=1`.
* **The bodies are still GL.** This gate ports the casters; a replacement mesh's own pixels are
  drawn by `tagpu_hires_draw.c` on both lanes and are not in the unit pass's picture at all,
  which is why the non-black count falls from 789 to 380 when the mesh is activated.


### 2.39 The UI markers, drawn by Vulkan (`tagpu_vk_mark.c`, `tagpu_mark.c`'s draw list) — landing 5 of the Vulkan-only plan

**MEASURED 2026-09-17.** The last engine-anchored layer in the viewport: health bars, group
digits, order markers and their `ShowRanges` labels, the build-cursor footprint, the drag band box
and the captured post-fog layer. §2.34 took the UI's own surface; this is what is drawn *over the
world* and *under* nothing.

#### It is seven draws, not one, and that is why a draw LIST crosses

Every other ported pass hands over buckets and counts. This one cannot: `tagpu_mark_render` issues
seven draws in the engine's own order — order triangles, order lines at `ss` line width, order
labels, the health bars, the group digits, the post-fog layer, then the cursors — each with its
own `uText`/`uFog`, and the last two with fog forced off because the engine draws them after its
fog overlay and never darkens them. A consumer that re-derived which buckets were non-empty could
disagree with the pass that drew them, so `TAGPU_MKHAND` carries `TAGPU_MKDRAW { first, count,
lines, text, fog, tex }` records built **as the GL draw issues them**, and `mk_push` mirrors the
VBO byte for byte so every `first` recorded is the one GL used rather than a number re-derived
from the counts.

Nothing else needed a new mirror: the vertices are `tagpu_mark.c`'s own arrays, the captured layer
is already `TAGPU_MARKLAYER`'s CPU bytes, the text atlas was crossed in G19d
(`tagpu_text_atlas(&gen)`), and the palette, fog grid and fog LUT were crossed for §2.30–2.33.

#### The lever it owed, and paid before it was written

Every restored-art A/B on this plan arms `mark.on`, for a reason that has nothing to do with
markers: `tagpu_rglsl_step` runs from `tagpu_native.c` only when one of `fx|sfx|feat|terr|mark` is
armed, and `mark` was the only one of the five with no Vulkan pass of its own — so it stepped the
restorer while leaving exactly one pass drawing, which is what the capture requires. Porting it
made all five drawing passes and the recipe stops working. `tagpu_rglsl.step` is the replacement:
`tagpu_rglsl_step_forced()` polls it on a 30-frame cache and steps the restorer while arming no
pass at all, with a call-count compare so a frame already stepped is not stepped twice.

#### The fixture is the expensive part, and the four earlier runs failed on it

`tagpu_mark_render` returns before it draws anything when every bucket is empty, so the GL half of
the A/B never reaches the disk and there is nothing to diff. Four automated attempts ended there
identically. What the pass actually needs is listed in the `ta-drive` skill; the short form is that
**six of the seven draw kinds each have a separate gate**, and arming `mark.on` opens none of them:

| kind | what makes it non-empty |
|---|---|
| health bars | a unit of the watched player on screen, `damagebars` on |
| group digits | `u->squad` non-zero — `ctrl+<n>` on a selection, nothing else |
| order lines | `tagpu_order.on` **and** SHIFT physically held (the engine's driver at `0x469BFC` is shift-gated) **and** an order that does not complete — a `patrol`, not a `move` |
| order triangles | the marching route dots, which the engine draws only at `flag == 1`: the **hovered** unit. Game speed 1, or a slow unit walks out from under the pointer between `tacli roster` and `pmove` |
| labels | the `ShowRanges` console cheat, typed — `tacli switches` does not reach it |
| cursors | a **held** drag (`down:lbutton`, move, no release) or a build placement |
| post-fog layer | `mark.on=nocursor`, the one window in normal play that still fills it |

#### Two defects that the health bars alone could not show

The first A/B on this pass carried `bars=3` and nothing else and measured **0 px**. Extending the
fixture to six kinds took it to **4 066 px**, and two independent faults came apart behind it.

**The order lines rasterised under the wrong rule.** The pipeline declared
`VK_DYNAMIC_STATE_LINE_WIDTH` and never chained `VkPipelineRasterizationLineStateCreateInfoEXT`,
so Vulkan used its default mode where GL's non-antialiased lines follow the diamond-exit rule.
On 436 segments of route line and range circle that is **4 900 px** the Vulkan lane lit and the GL
twin did not, with the GL half a near-perfect subset of the Vulkan one. `tagpu_vk_fx.c` has
carried `VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT` for the same reason since G19e and this pass
did not copy it. The line pipeline is now built **only** when the device gave us `bresenhamLines`,
so `prepare`'s refusal is a refusal and not a fallback.

**The text atlas was uploaded every frame and never bound.** The GL twin feeds ONE sampler —
`uLayer` — from TWO textures, swapping the bind on unit 0 between the captured layer and
`tagpu_text.c`'s coverage atlas. Vulkan has no per-draw texture bind and this pass had one
descriptor set, whose binding 40 was always the layer. So every text draw sampled the layer — on a
frame with no captured layer, the 1×1 `0xFF` stand-in — `texture(uLayer, vUV).r` was 1.0 for every
fragment, the `< 0.5` discard never fired, and each label and digit came out a **solid filled quad**
in its vertex colour: **3 891 px**. There are now two descriptor sets per slot, identical but for
unit 0, and `record` picks one per draw. A draw naming a texture the hand-over did not bring is
refused outright, because binding 40 falls back to whatever image exists rather than leaving a
hole — the same bug by a second road.

#### And the frame is upside down — the whole lane, and the A/B could not see it

**[FOUND BY THE OWNER, LOOKING AT THE SCREEN, 2026-09-17.]** With the labels finally drawing, the
Route D window showed them mirrored top-to-bottom. It is not the text: **every ported pass draws
the whole frame vertically flipped**, and it has been true since G19e.

**This is not a fact nobody wrote down — §2.28 states it, and states it as correct**: *"Both
halves of this A/B are upside-down pictures of the world, and that is correct: the GL twin draws
into the world FBO, whose clip-space +1 is the bottom of the screen (the composite quad turns it
over). They are upside down identically, which is the only thing the comparison asks."* Every word
of that is true **of the comparison**. What it does not cover is the lane's own window, and the
reason it does not is that until this landing the Vulkan lane had no picture a human ever looked
at: Route D exists to be captured.

The two halves, then:

1. **The lane reproduces the GL world FBO, and there is no composite quad on the Vulkan side.**
   Every world and marker pass writes `gl_Position.y = p.y/uGame.y*2.0 - 1.0` on the engine's
   screen-space y, which grows *downward*, so clip +1 is the bottom of the game frame. GL's own
   composite turns the FBO over on the way to the window, which is why the game looks right.
   `tagpu_vk_fx.c`'s `vp.y = h; vp.height = -h` makes the Vulkan image match that FBO — and the
   ported passes draw **straight into the swapchain image**, so nothing ever turns it back. Eight
   files carry that viewport (`fps`, `fx`, `mark`, `feat`, `unit`, `scaffold`, `gui`, `terr`) and
   five carry a scissor rect mirrored to match it (`fx_scissor` and its four copies).
   `tagpu_vk_shadow.c` is the exception and always was: its header says *NO Y FLIP* because the map
   is an offscreen texture sampled by UV, never presented.
2. **`tagpu_abshot.c` turns the GL half's rows over**, because `glReadPixels` hands back the
   bottom row first and a PPM's first row is the top. For **this** framebuffer the bottom row is
   the game frame's TOP, so the reversal produces an upside-down PPM — which is exactly the pair
   §2.28 describes, and it is why the two halves line up.

So every A/B on this plan has compared two upside-down pictures. That is a valid comparison **of
content** — geometry, colour, coverage and ordering were all genuinely checked and those figures
stand — but two things fall outside it: the lane's presented picture, and any rasterisation rule
whose answer depends on which way up the viewport is. The 32 px below are the second of those.
Nothing culls (`VK_CULL_MODE_NONE` in all thirteen ported pipelines), so the winding argument the
flip is also justified by buys nothing.

**Proven, not inferred, in three steps.** An X capture of the real game window shows the labels
upright and the group digit *below* its bar; an X capture of the Route D window shows them mirrored
and the digit *above*. Rebuilding this pass alone with `vp.y = 0; vp.height = +h` put the Route D
window upright — and broke the A/B to **20 540 px with the non-black counts identical at 10 361 a
side**, which is a mirror and nothing else. Undoing the capture's row-reversal on that same pair
gives **0 differing pixels of 786 432**.

That last number is why the flip is filed as its own landing rather than noted: **the 32 px this
landing measures ARE the flip**, and they are the first thing on this plan that the matched
upside-down pair could not absorb. A horizontal line whose window
y is an exact integer floors to one row under GL and to the other under a mirrored viewport, since
`floor(h − y)` and `h − 1 − floor(y)` agree for every y except an integer. **And the order
markers' y lands on one half the time, for a reason worth writing down**: `tagpu_order.c`'s
`project` computes `sy = wz − walt·0.5 − eyeY + 32`, and for an order target `rec_pos` hands it
the 16.16 snapshot of an integer world coordinate — so `sy` is an exact integer when the target's
terrain altitude is **even** and a half-integer when it is **odd**. That is why some crosshairs in
the difference image tie and others do not, which "the engine projects them with integers" alone
does not explain. Vertical strokes are
unaffected (x is not flipped) and area primitives are unaffected (their sample points are at
half-integers and never on a boundary), which is exactly the pattern the difference image shows.

#### The numbers

Fixture `selbox-facings`, 1024×768, `ss=1`, zoom 1, `mark.on=log order.on=log vk.on=color=0,0,0`,
three ARMSTUMPs on a patrol with SHIFT held, one hovered, `ctrl+1` assigned and `+showranges` on.
The captured frame carries bars, route dots, route lines, range circles, nine labels and three
group digits — verified in the captured image itself, not from the `mark:` counter line, which
prints every 120 frames and is therefore the fixture's state and not the frame's.

| fixture / build | non-black GL | non-black Vulkan | differing of 786 432 |
|---|---|---|---|
| bars only — the first fixture | 297 | 297 | **0** |
| six kinds, as first built | 10 208 | 15 097 | 5 144 |
| + Bresenham lines | 10 266 | 14 157 | 4 066 |
| + the text atlas bound | 10 324 | 10 324 | **32** |
| …the same pair, **both** the viewport flip and `tagpu_abshot.c`'s row order corrected | 10 361 | 10 361 | **0** |
| the band box held open (`cursor=8`) | 2 456 | 2 456 | **0** |
| the captured post-fog layer (`nocursor`, `postfog=captured`) | 2 456 | 2 456 | **0** |

**Read that fifth row carefully: it takes TWO changes, not one.** Correcting the
viewport alone leaves the A/B at **20 540 px** — the Vulkan half is then upright and the GL half
is still reversed, which is a mirror and nothing else. 0 px is what the pair measures once
`tagpu_abshot.c` stops turning the GL rows over as well. [Caught by the landing review, which read
the table against the prose above it.]

**And the GL column moves on every row, including rows whose only change was Vulkan-side**, because
the fixture is live — three units on a patrol, a pointer hovering one of them — and is not
frame-reproducible. Each row's own diff is a paired measurement of one frame and is evidence;
differences *between* rows' GL counts are not.

#### Not covered

* **The cursor bucket and the post-fog layer are no longer open — both closed at 0 px**, as
  the last two rows of the table. They take a band box held open across the capture
  (`down:lbutton`, move, and no release until after), and they are mutually exclusive by lever
  because `nocursor` is what fills the layer — so they are two captures, not one. **All seven draw
  kinds are now measured.** Both come out 0 px even with the flip in place, and that is the
  tie-break argument confirmed from the other side: the band box is built by `put_outline` →
  `put_bar` → `put_barf`, which emits triangle quads, and the layer is a textured quad. An area
  primitive's coverage is decided at sample points on the half-integer grid, which never land on a
  pixel boundary, so mirroring cannot move one. Only a zero-width line can tie.
* **One map, one resolution, one GPU, one OS, `ss=1`, zoom 1.** The `ss != 1` line width is
  *refused* rather than drawn, so it is a bound and not a gap; zoom is untested either way.
* **The flip above is diagnosed and proven and NOT fixed** — it is nine files and every pass's A/B
  has to be re-run, which is a landing and not a rider on this one.

### 2.40 The lane's frame was upside down, and no A/B could see it — landing 5b of the Vulkan-only plan

**FOUND BY THE OWNER, LOOKING AT THE ROUTE D WINDOW, 2026-09-17**, while landing 5's labels were on
screen for the first time. The text read mirrored. It was not the text: **every world pass drew the
whole frame upside down**, and had since G19e.

#### Two y conventions, and the flip is right for one of them

This tree's GL shaders do not agree about which way y runs, and that is not sloppiness — they draw
into different targets:

| convention | passes | where the GL twin draws | Vulkan needs |
|---|---|---|---|
| `p.y/uGame.y*2 − 1` on the engine's screen-space y, which grows **downward** | `terr`, `feat`, `fx`, `unit`, `mark` | the **world FBO**, whose clip +1 is the bottom of the game frame; GL's composite quad turns it over on the way to the window | **no flip** — clip −1 is already the game's top row, which is row 0 under Vulkan |
| `1 − y*2`, or an NDC rect built y-up | `gui`'s composite, `fps`, `scaffold` | GL's **window**, in GL's own convention | **the flip** — a negative viewport height, because the two APIs disagree about which row of a window is the top |

The lane applied `tagpu_vk_fx.c`'s negative viewport height to **all** of them. For the first group
that is a second turn, and the ported passes draw **straight into the swapchain image** — there is
no composite quad on the Vulkan side to turn it back. So Route D presented the world upside down.

`tagpu_vk_shadow.c` was never affected and its header already said why: its map is an offscreen
texture sampled by UV, never presented, so it takes a positive height. That was the one pass whose
author asked the question this section is the answer to.

#### Why eight landings of A/Bs could not see it

`tagpu_abshot.c` turned the GL half of every capture over by the same rule, because `glReadPixels`
hands back the bottom row first and a PPM's first row is the top — which is correct for a
default-framebuffer readback and wrong for the world FBO, whose first row IS the game's top row. So
both halves were mirrored and they lined up exactly.

**This was written down and accepted, in as many words.** §2.29: *"Both halves of this A/B are
upside-down pictures of the world, and that is correct… They are upside down identically, which is
the only thing the comparison asks."* Every clause is true **of the comparison**. What none of it
covers is the lane's own presented picture — and until landing 5 the Vulkan lane had no picture a
human ever looked at. Route D exists to be captured.

**The general lesson, and it is the one worth carrying:** an A/B compares the two lanes **to each
other**, so it is structurally blind to any error they share. Every figure on this plan stands as a
*content* comparison — geometry, colour, coverage and ordering were all genuinely compared — but two
classes fall outside it: the lane's presented picture, and any rasterisation rule whose answer
depends on which way up the viewport is. **The screen is the oracle for those, not `vk-ab.py`.**

#### What changed

* **Five viewports**, `vp.y = h; height = −h` → `vp.y = 0; height = +h`.
* **Five scissor rects** that were deliberately mirrored to compensate (`fx_scissor` and its four
  copies): `y0 = h − (ytop + hh)` → `y0 = ytop`. They are the same rectangle on both sides now.
* **`TAGPU_ABSHOT_TOPDOWN`**, set by exactly those five passes' GL halves. It is a flag and not a
  blanket change because the reversal stays right for `gui`, `fps` and `scaffold`. With the flag
  clear, `write_ppm` emits rows `h−1 … 0` — byte-identical to the loop it replaced, so the change
  is a provable no-op for those three.
* **Four `flipok` refusals deleted.** `VK_KHR_maintenance1` was needed *only* for the negative
  height — `tagpu_vk.c` says so itself ("*it buys exactly one thing*"), and nothing in the lane uses
  `vkTrimCommandPool`, a 2D-array view of a 3D image, or `VK_ERROR_OUT_OF_POOL_MEMORY`. So `terr`,
  `feat`, `fx` and `unit` no longer stand down without it; `gui`, `fps` and `scaffold` still require
  it and still ask.
  **AND THAT CHANGES WHAT A DEVICE WITHOUT IT SEES.** Before, no ported pass could flip and none
  drew, so Route D presented the clear colour. Now the five world passes draw and the three overlay
  passes stand down — **a world with no UI over it**. That is not a regression by this project's
  rule (each pass still reproduces its own part or refuses), and it is the same shape as the
  shipped configuration in §2.35, where the lane draws the UI and nothing else. It is written down
  here and in the seam's own log line because it is a behaviour nobody chose deliberately.
* The prose that argued for the flip, in five pass headers, in `tagpu_vk_pass.h`'s `flipok`
  contract, and in four places in this file — including three `NO CULLING` comments whose stated
  reason was the winding the flip reversed. Nothing culls and nothing should; only the
  justification moved.

#### How it was verified — and the method is the point

**The screen, not the A/B.** Both windows are captured by **window id** and diffed:

```
DISPLAY=:0 xwininfo -root -tree | grep 1024x768     # the TITLED window is the game;
DISPLAY=:0 import -window <id> out.png              # its untitled sibling at the same +X+Y is Route D's
```

Two traps, each of which cost a run:

* **Two instances park at the same coordinates.** `park.sh` puts every window in the same place, so
  "the untitled sibling at that position" can belong to a *different* instance. Stop all but one.
* **An obscured window's backing store is stale.** Route D covers the game completely, so an
  `import` of the game window returns whatever it last held — in one run, the pre-scenario frame,
  which read as a 534 730-px difference that was really two different moments. Capture the game
  **before** the lane is armed, on a static fixture.

**Each of the three that KEEP the flip was established on the screen separately**, because "its
shader says so" is the inference this landing exists to distrust:

* **`gui`** — with every pass armed, the difference between the two windows was *exactly* the world
  viewport, `(128,32)–(1024,736)`. The side panel, the top bar and the bottom bar were identical
  pixel for pixel, which is the composite reproducing them upright while the world was mirrored.
* **`fps`** — the readout renders `FPS451` in Route D's top-left, upright and legible, at the
  corner the GL window puts it in. A mirrored readout is unmistakable and this is not one.
* **`scaffold`** — the overlay tints its silhouettes by row, blue at the top to red at the bottom.
  Fitting `d(R−B)/dy` over the tinted pixels gives **+0.0166** in the GL window and **+0.2088** in
  Route D's: the same sign, so the same way up. (The GL slope is shallower because the tint is
  composited over terrain there; only the sign is the evidence.)

| measurement | result |
|---|---|
| GL frame vs Route D, every pass armed | **311 px of 786 432** |
| the same pair, Route D mirrored | 624 839 px |
| `terr` A/B | **0 px** of 786 432 (630 708 non-black a side) |
| `feat` A/B | **0 px** (103 638 a side) |
| `unit` (`posedraw`) A/B | **0 px** (2 125 a side) |
| `fx` A/B, `fx-rockets` | **0 px** (1 298 a side) |
| `gui` A/B — **untouched control** | **0 px** of 307 200 (306 937 a side) |
| `mark` A/B, six draw kinds | **0 px** (10 305 a side) — **was 32 px**, and those 32 were this |
| `mark` A/B, the band box | **0 px** (2 456 a side) |
| `mark` A/B, the post-fog layer | **0 px** (2 456 a side) |

**[THE FIGURE ABOVE WAS 1 394 px UNTIL 2026-09-17 AND THE METHOD WAS WRONG.]** The first version of
this comparison captured the game window *before* arming `vk.on`, because Route D covers the game
window completely. **Arming the lane after launch starves it**: the mirrors it samples are asked for
on the 30-frame arm poll, and a lane that comes up mid-session never gets the ones established at
launch. Measured, on `one-unit` with `native.on` alone and nothing else moved: **382** non-black
pixels in Route D with `vk.on` armed at launch — the commander, matching that fixture's A/B exactly
— against **89** with it armed late, where the unit's body is simply absent. That artefact read as
"the Vulkan lane draws a commander black", and it is not a rendering fault at all. The corrected
method reads the GL side with `tacli glshot`, which goes to the GL lane rather than to an obscured
window, and leaves `vk.on` armed from launch.

**The 311 px are described rather than fully attributed**, which the 1 394 were not. They sit in
four 64-px cells — x 516..732, y 363..398 — on two of the fixture's three parked ARMSTUMPs, at a
worst channel of **75**: a shading difference on two units, not a missing or displaced object.
**Nothing outside the world viewport differs at all**, so the GUI layer is pixel-identical. Two
plausible contributors and neither is separated here: this run used the **play** anisotropy default
rather than the `aniso=1` substitution §2.37 established for unit comparisons, and the two captures
are still not the same instant. The same-frame check on this fixture is the unit pass's own A/B,
which reads **0 px**.

#### What it closed

**Landing 5's 32 px were this, and the prediction was made before the measurement.** A horizontal
line whose window y is an exact integer floors to one row under GL and the other under a mirrored
viewport, because `floor(h − y)` and `h − 1 − floor(y)` agree for every y but an integer. With the
flip gone, the same fixture with the same six draw kinds measures **0 px** — and the two marker
fixtures that were already 0 with the flip in place (the band box, the captured layer) are still 0,
which is the other half of the argument: both are AREA primitives, whose coverage is decided at
sample points on the half-integer grid, and mirroring cannot move one of those.

#### Two fixture defects found on the way, neither caused by this landing

* **Gate 2's feature A/B recipe has been unusable since gate 3a.** `feat` needs `native.on` armed to
  own its leaf, and gate 3a is what made the Vulkan **unit** pass draw — so the seam now refuses
  with *"1 A/B levers claimed this frame and 2 passes drew into it"*. Gate 2 could take that
  measurement only because the unit pass was still standing down on the atlas mirror. The workaround
  is to pan the camera off every unit (`tacli eye`) until `native: 0 unit(s)`, which leaves `feat`
  the only Vulkan pass drawing.
* **`fx-lasers` does not reliably fire.** Two runs measured `proj=0 laser=0` at the capture frame, so
  the GL half never reached the disk and the A/B produced nothing. `fx-rockets` holds model
  projectiles in flight for minutes and is the fixture to use. This is the "a 0 px from a fixture
  that never took the branch" trap in its better form: no result rather than a false pass.

#### And one premise it turned over on the way

`tagpu_vk_unit.c` stood the unit pass down on any frame whose twin had the scaffold overlay armed,
because `TAGPU_GLSL_SCAF_TEST` reads `gl_FragCoord` and *"this pass's flipped viewport makes the two
exact mirrors"*. **That reason is now inverted**: `tagpu_glsl.h` states that the GL VS maps game row
0 to FBO window y 0, so GL reads `g + 0.5` for game row g, and with a positive height this lane
stores game row g at image row g and reads `g + 0.5` too. The two agree. The paragraph ended *"any
later pass whose fragment shader reads `gl_FragCoord` inherits this"*, so it is corrected in place
rather than deleted. [Found by the landing's review.]

**The refusal itself stays, on the half that is still true**: `tagpu_vk_scaffold_view` returns
VK_NULL_HANDLE for a frame or slot that is not its own and `bind_main` then binds a 1×1 stand-in,
which against a twin sampling a real overlay is a different picture. Gating the refusal on the view
rather than on `scafOn` is now a small bounded change — but it enables a path this lane has never
measured, and it is left for the landing that measures it.

#### Not covered

* **The `fx` figure above is model projectiles and sprites, not lines** (`lines=0` in that frame).
  §2.31's 3–6 px tie-break residual was measured on a laser **bolt**, and the argument there is the
  same mirroring this landing removed — so it should be closed too, but that is **reasoned, not
  measured**, and it needs a laser fixture that actually fires.
* **One map, one resolution, one GPU, one OS, `ss=1`, zoom 1.**

### 2.41 The build ghost, carried at last — landing 6 of the Vulkan-only plan

**MEASURED BEFORE IT WAS STARTED, AND THE COST WAS TOTAL.** The plan's row read *"not reached today
— the unit pass refuses on the atlas mirror several checks earlier — so its cost is still
unknown"*. Gates 3a and 3b removed that earlier refusal, so it is reached now, and the first thing
this landing did was find out what it costs. With one ARMCOM selected and a solar placement open
(cursor mode `0x0E`, confirmed by peeking `main+0x2CC3`):

```
ghost: curs=6193 queue=0 drawn=6193 nobake=0 trunc=0 alpha=0.40
vk: unit: the GL twin drew 1 posed unit(s) this hand-over does not carry
          (a build ghost, or past its cap) - nothing drawn while that is true
```

**For as long as a building placement was open the Vulkan unit pass drew nothing at all** — not the
ghost, and not the units either. `tagpu_ghost.on` is a play default gated on `tagpu_native.on`
(`tagpu_opt.c`), and placing buildings is most of what a TA player does, so this was the ordinary
case and not a corner. That is gate 3a's lesson applied rather than quoted: measure the refusal
before porting the stream behind it.

#### It is a change to the publish window, not a removed exclusion

The obvious reading of `pd_record` — `if (!s_recording || u->ghost) { s_other++; return; }` — is
that ghosts are excluded by that one clause. Dropping it changed **nothing**, and the verification
still read `otherDraws = 1`. `ghost_pass` opens a **second** `tagpu_posedraw_begin`/`_end` window,
and `s_recording = (s_win++ == 0)` meant only the **first** window of a frame recorded, so a ghost
never reached that clause at all.

Every window records now. The first is still the one that publishes the view and the one the A/B
brackets — those are separate questions from which windows record, and conflating them is what hid
this for as long as it was hidden.

#### The ghost is the same draw, which is why the consumer is one pipeline

A ghost rides `tagpu_posedraw_unit` with the same uniforms as a unit. It differs in exactly two
things: `alpha` (0.40, which `pd_record` already carried) and that the GL twin brackets its ghosts
in `glDepthMask(GL_FALSE)` — *"the ghosts blend with each other"*, while units drawn earlier still
occlude them, because the depth TEST stays on. So `tagpu_vk_unit.c` gains `s_pipeGhost`: the body
pipeline with `depthWriteEnable = VK_FALSE` and nothing else moved, bound per draw kind. The
records arrive units-then-ghosts because `pd_record` appends as each draw is issued, so in practice
it switches once.

#### A caster the GL depth pass never drew, caught before it shipped

`ghost_one` builds its record with `memset(&q, 0, sizeof q)` and then sets `alpha` and `ghost`, so
**`castSkip` stayed 0**. That was harmless for exactly as long as ghosts were refused outright —
but `pd_record` computes `casts = (s_depthOn && !u->castSkip)`, so the moment ghosts are carried
every one of them is handed over as a **caster**. That count is what `tagpu_vk_unit_casters` feeds
to the shadow census, which is compared against the GL side's own `otherCasters`; an extra caster
there is a **wrong shadow map, not a missing one**. `ghost_one` now sets `castSkip = 1`, which is
simply true: the depth loop runs earlier in the frame and over the real units only.

#### The review found two defects in the above, and both were real

**THE GHOST COMPOSITED ON THE WRONG SIDE OF THE EFFECTS.** `tagpu_vk.c` records terrain, features,
**units**, effects, markers — and carries a comment at that very line saying *two passes that blend
are not commutative*, which is why the body stage sits where it does. `tagpu_native.c` draws the
units, then `tagpu_fx_render`, and **then** `ghost_pass`. Recording the ghosts inside the body stage
therefore put them BEFORE the effects on this lane and after them on the GL one. Both pipelines
blend premultiplied `over`: GL yields `G over (F over D)` and Vulkan `F over (G over D)`, which
differ by up to the ghost's own alpha share of the effect — 0.40·F per pixel — for any translucent
effect overlapping a ghost, which is nano spray on a queued site or an explosion under the placement
cursor. The fixture below has one commander and nothing firing, so the measurement could not see it.

Fixed as a **second record stage**, `tagpu_vk_unit_record_ghosts`, called immediately after
`tagpu_vk_fx_record`. Standing the pass down whenever an effects pass and a ghost coexist was the
alternative and it defeats the landing: effects are armed in all ordinary play.

**AND THE A/B CLAIM WAS DROPPED ON FRAMES THAT DESERVED IT.** The first version read
`s_pub.ab = s_nghost ? 0 : s_abFrame;` followed by `s_abFrame = 0;`, in a publish block that runs at
**every** window's `_end`. The second window re-ran it with the value already consumed, so the pair
went unclaimed **whether or not a ghost had been recorded** — and `s_abDone` had latched, so the
one-shot never retried and the instrument read as a port failure for the rest of the session. It is
reachable on a frame whose queued build sites are all off screen, because `ghost_pass` opens its
window on the site COUNT and culls each site afterwards. A second path was worse: with no posed unit
on screen `tagpu_native.c` skips the unit window entirely, so the **ghost's window is the first**,
and the capture was bracketed around the ghost pass itself.

Both halves are fixed: the bracket opens only on a **non-ghost** window (`tagpu_posedraw_begin_ghost`
is a separate entry point, because counting windows is exactly the test that fails), only the window
that opened it closes it, and the claim is frame-scoped and idempotent.

#### The A/B cannot be this landing's oracle, for two independent reasons

1. **The ghost needs the marker pass.** It arms on `tagpu_mark_cursor_ours()`, so it draws only
   when `mark.on` owns the cursor layer — and the marker pass then draws the placement square.
   Two Vulkan passes in one frame, which the capture refuses outright, and there is no way to keep
   the ghost while removing the square. (`markown` installs its detours at DLL attach, so a
   `mark.on` written to a running instance opens nothing; `ghost: curs=0 drawn=0` with the cursor
   mode already 14 is that signature, and it cost two probe runs.)
2. **The GL half cannot contain a ghost.** It is blacked and read back around the **first** window,
   and the ghost draws in the second — while the Vulkan half is the whole presented image, which
   can. The two halves would differ by the ghost and the difference would be the instrument's.

So the pass **declines to claim the pair** on any frame that recorded a ghost, rather than report
that difference: `s_pub.ab = s_nghost ? 0 : s_abFrame`. Landing 6's oracle is §2.40's two-window
comparison instead, which needs neither a bracket nor a single drawing pass — the second time that
method has paid for itself, and the reason it was worth building.

#### Measured with §2.40's method, and the ghost's own pixels are exact

`one-unit`, 1024x768, `ss=1`, `native.on=all wrecks` + `ghost.on` + `mark.on` + `terr.on` +
`feat.on` + `gui.on`, `vk.on` armed **from launch** (§2.40's trap), the GL side read with
`tacli glshot`, a solar placement held open across both captures.

| | |
|---|---|
| cursor mode | `0x0E`, and `ghost: curs=11998 drawn=11998` |
| `vk: unit: … nothing drawn while that is true` | **0 occurrences** — the stand-down is gone |
| GL frame vs Route D, a ghost in both | **260 px of 786 432** |
| **inside the ghost's own box** (560..650, 380..470) | **0 px** |

**The ghost is reproduced exactly.** The 260 are two 64-px cells elsewhere — x 495..532,
y 449..493, worst channel 184 — on the ARMCOM's own body, which is the same class of residual as
§2.40's 311 px on two parked ARMSTUMPs and has the same two unseparated contributors: this run
used the **play** anisotropy default rather than §2.37's `aniso=1` substitution, and the two
captures are not the same instant.

#### Not covered

* **The queued-build ghosts are not exercised.** `ghost: queue=0` in every run here: the fixture
  opens a placement but never commits one, so only the cursor ghost drew. The queue path goes
  through the same `ghost_one` and the same record, so it is carried by construction, but that is
  an argument and not a measurement.
* **AN EFFECT OVERLAPPING A GHOST IS NOT MEASURED.** The composite-order fix above is by
  construction — the ghosts are recorded in a stage after `tagpu_vk_fx_record`, which is where the
  GL twin draws them — and the fixture here has nothing firing, so no measurement here distinguishes
  the fixed order from the broken one. A fixture that puts nano spray or an explosion under a
  placement cursor would, and none exists.
* **The A/B paths repaired above are reasoned, not re-run.** The frames that exposed them — all
  queued sites off screen, and no posed unit on screen with a placement open — are not in any
  fixture either.
* **One map, one resolution, one GPU, `ss=1`, zoom 1, one building type.**

### 2.42 The restorer comes apart along the line that is not the API — landing 7 of the Vulkan-only plan

**The split was mandatory and the forcing fact is in the tree, not in the plan.**
`tagpu_restoreglsl.c` was one 1 132-line file doing two unrelated jobs: an incremental background
**scheduler** — job queues, batch formation, a per-slice GPU-time budget driven by a smoothed
cost-per-unit estimate — and a **GL draw sequence**. Landing 11 deletes `opengl_utils.h`, which the
file includes. But `tagpu_rglsl_tileable` is called from `tagpu_terr.c:1034` and
`tagpu_gaf.c:1130`, both *gather* halves that survive the deletion, and the job queues are what
every consumer's lazy-restore contract is written against. So the module could neither be deleted
with GL nor left as it was.

**It was done now because landing 4 removes the oracle.** After Route D goes there is no way to
show that a refactor of the GL restorer changed no pixel, and a scheduler refactor whose
correctness is never demonstrated is exactly the kind of silent wrongness this stack produces.

| file | what it owns |
|---|---|
| `tagpu_restore_core.{h,c}` | the weight file, the options, the size-class ladder, `tileable`, the job table and its queues, batch formation, the **pass sequencer**, the cost model, the budget arithmetic, every counter, and every log line but two (below). Names no rendering API. |
| `tagpu_restoreglsl.c` | device resources and the three draws, behind a twelve-entry backend interface. |

**The backend is TOLD which draw to make rather than working it out.** `issue_draw` in the core
decides fill / conv / out, advances `pass`, `group` and the ping-pong side, builds the per-slot
tables and the out-pass vertices, and computes the draw's cost in work units; the backend binds and
draws. That is the whole reason the line is drawn there: a second backend that re-derived the
sequence could disagree with the first about which layer it was on, and **no A/B would show it** —
both lanes would be internally consistent and produce different pictures for a reason neither
reports.

**Two things were kept deliberately rather than tidied, and both would have been quiet regressions.**

* **One scheduler per backend, not one shared.** A shared budget is defensible on its merits — the
  total restore work per frame is what matters to the frame — and it would also have changed the GL
  lane's slicing the moment a second lane came up. This split is meant to be pure code motion, so
  each backend declares its own `TAGPU_RSCHED`.
* **The options stay per-CONTEXT.** They were read inside `init_gl`, so a `tiny` or `budget=`
  edited between two GL contexts took effect, and the `ta-drive` skill documents exactly that
  (*"read once per GL context… the startup GL reset re-reads them once, a map change does not"*).
  `tagpu_rcore_reload` is called from a backend's own init to preserve it. Making the model and
  options process-wide is the natural-looking mistake and it would have silently broken a
  documented knob.

**Two diagnostics were reworded, and that is the one place "pure code motion" is not literally
true.** The core cannot say *"timer query"* or *"MAX_UNIFORM_BLOCK_SIZE"* — both are GL terms, and a
file that names no API must not assert them — so those two lines now read *"the GPU timer never
completed"* and *"uniform block %d < one k-block"*. Neither string is referenced by any note, skill
or tool, and **every line that IS documented is byte-identical**, because the lane name the core
prefixes with is `"restoreglsl"`: `lazy restore armed`, `job started`, `done:`, `queue drained`,
`idle: activations freed` and the `%dx%d %s, NK=%d …` banner all keep their exact text.
[The overclaim was *"every counter and every log line"*, caught by this landing's review.]

**MEASURED: the GL restorer's output is byte-for-byte identical.** `tagpu_restoredump.on` writes
the finished terrain atlas with `glGetTexImage`, so the comparison needs no window and no capture.
Same instance, same map, the two DLLs swapped under `--keep-dll`:

| fixture | dump | pre-split | post-split |
|---|---|---|---|
| `static-terrain` | `tagpu_restore.rgba` | 46 461 952 B (2176x5338, 10 036 tiles) | **identical, `cmp` clean** |
| | the `done` line | 10 036 frames (4 142 wrap-padded), 158 batches, 7 426 draws | **the same four counts** |
| `feat-forest` | `tagpu_restore.rgba` | 23 674 880 B (2176x2720, 5 062 tiles) | **identical** |
| | `tagpu_restore_unit.rgba` | 16 777 216 B | **identical** |
| | `tagpu_restore_unit.r8` | 4 194 304 B | **identical** |
| | `tagpu_restore_unit.idx` | 427 B, 25 entries | **identical** |
| | the `done` line | 5 062 frames (400 wrap-padded), 80 batches, 3 760 draws | **the same four counts** |
| | `unit: queue drained` | 25 frames in 2 batches, 94 draws | **the same three counts** |

**EVERY FIGURE ABOVE WAS RE-TAKEN AFTER THE REVIEW'S FIX AND AGREES WITH THE FIRST RUN** — all
five dumps identical again, both `done` lines' counts unchanged, the `queue drained` tally
unchanged. The guard added for the review's one real finding cannot fire on the GL lane, which is
an argument; this is the measurement, and the number now describes the code that landed rather than
the code the reviewer read.

**Two fixtures on two different maps, and the second one is the one that matters most**: the
terrain's job is a fixed list added once, but a GAF atlas's is an **open queue** fed on every miss,
and that is where `job_add` while a run is live, the size-class mixing, the batch-boundary re-pick
and the `queue drained` tally all live. The unit atlas's twin is a product of that path and it came
back byte-identical, index file included.

The wall time and fps differ between the runs (10 337 ms at 122.9 fps against 11 083 ms at
122.2 fps on the first fixture; 4 835 against 4 776 ms on the second) and **that is not a finding**
— the slice budget is wall-clock-driven, so the number of slices a restore takes, and therefore how
many *frames* it is spread over, is a property of the machine that hour rather than of the code.
The counts that *are* code-determined — frames, wrap-padded, batches, draws — match on both
fixtures, and every byte of every atlas matches.

**The idle path is on the record too**: `restoreglsl: idle: activations freed` appears in both
runs, so the 180-slice release fires through the new `act_free` return-value contract.

**THE REVIEW FOUND ONE REAL DEFECT AND IT WAS IN THE NEW INTERFACE, NOT IN THE MOVED CODE** —
which is the right place for it to be, and the reason a refactor gets reviewed at all.
`tagpu_restore_core.h` promised `tagpu_rcore_job_new` returns NULL *"when the table is full **or the
model is unusable**"*, and the code never looked at the model; `tagpu_rcore_ready()`, added in the
same commit for apparently that purpose, was called from nowhere. Without the guard a backend that
creates a job before loading weights gets a live job with `depth == 0`, and the sequencer then walks
off the front of the model: FILL sets `pass = 1`, `pass <= depth` is `1 <= 0`, so the next draw
takes the **OUT** branch and evaluates `layer[depth - 1]` — `layer[-1]`, the four ints in front of
the array, read as an input-tile count.

**The GL lane cannot reach it** (`job_new_x` runs `init_gl` first, which fails on a bad weight file),
so it is zero risk today and a **trap laid for the second backend — which is the entire reason the
interface exists**. Fixed by making the header true. Two dead additions went with it (`s_modelTried`,
written and never read; `TAGPU_RDRAWREQ.cols`, set and never read) and the draw request now states
the tables' actual lifetime: rebuilt on each batch's FILL, same contents for that batch's CONV and
OUT.

The reviewer separately confirmed, against the pre-split file, that the sequencer advances on the
same side of the draw in the same order in all three branches; that the conv cost really does use
the full `NK` rather than the clamped tail; that `pick_job`'s inflight-first loop means the slot
tables can never describe another job's batch; that `state_push`/`state_pop` are balanced across
every early return; that `s_sched.timer` and `s_query[0]` cannot disagree across `glreset`, a failed
re-init or the give-up path; and that a `draw` returning 0 cannot spin.

**What this measurement does NOT cover**, stated rather than implied:

* the **failure** paths — `act_ensure` returning 0 (no float render target), a destination that is
  not a complete render target, the out-of-memory returns. None is reachable on a working device
  and none is exercised.
* the **timer give-up** path: 300 slices without a query result, which needs a driver that accepts
  a timer query and never completes one.
* `job_clear` and `repalette` — the palette-moved-under-a-live-job path. `tagpu_gaf.c` reaches
  both on a palette change, which neither fixture produces.
* **the feature and effects queues were never FED, which is not the same as never drained.** Both
  logged `lazy restore armed` on `feat-forest` and then produced **zero batch lines** with
  `restoreglsl.on=log` on — so `tagpu_rcore_job_add` was never called for either and the `feat` and
  `fx` jobs are covered only as far as `job_new`. The reason is the fixture: those two atlases hold
  **GAF sprites**, and `feat-forest`'s trees are 3DO features, so what they populated was the
  *unit* atlas — which is the one that did get verified. Closing this needs a fixture with real
  2D sprite art in view (`fx-rockets` for the effects side). They run the same queue and batch
  path as the unit atlas, which is an argument and not a measurement.
* two GPUs' worth of nothing: one GPU, one model, NK=4, fp32.

### 2.43 The restorer restores on Vulkan, and two batches in one slice cost six cells — landing 7c of the Vulkan-only plan

§2.42 split the restorer along the line that is not the API and left `tagpu_vk_restore.c` complete,
warning-free and **with no consumer**, which is to say dead. This is the consumer, and it is the
terrain's tile atlas.

**What crosses the hand-over is the WORK, not the picture.** Under `tagpu_restorevk.on`
`tagpu_terr.c` stops reading its own restored twin back for the Vulkan lane — the CPU mirror gate 2
built — and publishes the **frame list** instead: the very array it built for its own GL job, in the
order it built it (`TAGPU_TERRHAND::restoreFrames/restoreN/restoreSerial/restoreRepaint`). The
Vulkan lane then paints its own restored atlas.

The list rather than a "restore it" flag, because three of a frame's eleven numbers are
engine-memory facts and belong on the gather side of the split: `wrap` is
`tagpu_rglsl_tileable()` over the tile's own texels against the **art** palette, and the order is
the centre-out rank over the live tile map (`restore_order`, `tagpu_terr.c`). What crosses is their
result. That is also what makes the two lanes comparable byte-for-byte rather than merely
both-plausible: both restore **the same rectangles in the same order from the same atlas with the
same palette, in the same process, on the same frames**.

**The reach into files this landing does not own is one usage flag.** `mk_image` and
`shared_resize` in `tagpu_vk_terr.c` now take the caller's `VkImageUsageFlags`, because the restored
atlas is rendered into and a colour attachment must say so at create time; the other four images
there are only ever sampled and say so. `COLOR_ATTACHMENT` is unconditional on the restored atlas
rather than lever-dependent — RGBA8 optimal-tiling colour-attachment support is required of every
Vulkan device, and paying for it always keeps the image's identity independent of which path filled
it.

#### The measurement, and the bug it found

**Both lanes restore in one run and the dumps are one `cmp` apart.** `tagpu_terr.c` writes
`tagpu_restore.rgba` off its restored **texture** with `glGetTexImage`; `tagpu_vk_terr.c` now writes
`tagpu_restore_vk.rgba` off the image the Vulkan restorer painted. The Vulkan half does not stall
the device to get the bytes: the copy is recorded into the frame's command buffer and read at **that
slot's next `prepare`**, the one instant the seam's fence has proved the submit carrying it
completed — the argument `shared_slot_done` in the same file already makes for the retire.

`static-terrain`, `full` model, NK=4, fp32, both dumps **46 461 952 bytes** (2176 × 5338 RGBA):

| | GL lane | Vulkan lane |
|---|---|---|
| frames | 10 036 (4 142 wrap-padded) | **10 036 (4 142 wrap-padded)** |
| batches | 158 | **158** |
| draws | 7 426 | **7 426** |
| slices | 853 of 5 319 frames | 918 of 1 276 frames |
| GPU measured | 11 373 ms over 100 % of the work | 12 602 ms over 100 % of the work |

Every code-determined count is identical. **And 6 936 texels of 11 615 488 — 0.0597 % — differed**,
every one of them transparent black on the Vulkan side and coloured on GL's.

**6 936 is exactly 6 × 34², and 34 is the atlas cell pitch.** Clustered by cell, the difference is
**six whole cells of 10 036, each entirely unpainted, and every other cell byte-identical** — not
noise, not a flip, not a rounding difference. That shape names its own cause.

**THE CAUSE: a slice can issue more than one batch, and the OUT draw's vertices were staged per
FRAME SLOT.** `tagpu_rcore_step`'s loop re-picks at every batch boundary and runs until the
GPU-time budget is spent, so batches per slice is bounded by a **time budget and not by a count**;
the terrain's own log shows **batches 157 and 158 both issued at slice 917**. Both OUT draws wrote
their vertices to `s_vmap + s_slot * sizeof(verts)` — the same address — so batch 158's 36 vertices
landed on top of batch 157's 384 before either draw executed. Batch 157's draw then painted batch
158's six destinations with batch 157's slot content, batch 158's own draw repainted those six
correctly, and **batch 157's leading six cells were never painted at all**. Six cells, silent, in a
picture that is otherwise byte-perfect.

**The GL lane has no such hazard and that is why the port did not inherit one.** A
`glBufferSubData` followed by a draw is ordered by GL itself — the driver renames or copies. A host
write to a mapped Vulkan buffer is ordered by nothing.

**The fix is an ordering, and a bigger arena is explicitly NOT it.** Since batches per slice is a
time budget rather than a count, *any* arena is a number that can be exceeded, and what it buys when
it is exceeded is this same silent failure. `vkCmdUpdateBuffer` records the data **at that point in
the command stream**, so each batch carries its own copy and the ordering is the command buffer's
own; the buffer became one batch's worth (12 288 bytes, well inside the command's 65 536 limit) and
device-local instead of eight host-mapped slot regions. The barrier around it is deliberately **both
ways**: `TRANSFER → VERTEX_INPUT` makes this batch's vertices visible to its own draw, and
`VERTEX_INPUT → TRANSFER` makes the previous batch's read of those bytes a fact before this write
lands on them.

**Re-measured after the fix, and again after the review's four fixes** (three runs in all, the last
on the landing as it stands): **`cmp` CLEAN — both dumps 46 461 952 bytes, byte for byte
identical**,
and the counts still 10 036 frames / 4 142 wrap-padded / 158 batches / 7 426 draws on both lanes.
**The collision condition was exercised again in that run** — batches 157 and 158 both issued at
slice 914 — so this is the fix holding under the case that broke it, not the case failing to occur.
That also settles §2.42's recorded-but-untrusted **no-flip derivation empirically**: a flip would
show as every cell's rows reversed, and 46 MB agree.

#### Two more ordering holes, found by the act of wiring a consumer rather than by a reviewer

Both were invisible while the pass had no consumer, and both are the class this stack fails at
silently.

1. **None of the restorer's render passes declared a subpass dependency.** The implicit dependency
   Vulkan adds at `dependencyCount == 0` has `dstStageMask = BOTTOM_OF_PIPE` and
   `dstAccessMask = 0`: it orders the attachment's **layout transition** and nothing else. So FILL's
   writes were unordered against the first CONV's sample of them, every CONV against the next, and
   OUT's write of the consumer's atlas against the consumer's own sample of it one render pass
   later. Drivers that flush at a render-pass boundary hide it, which is the problem and not the
   fix. Each pass now states both ends, and because a `0 → EXTERNAL` dependency's destination scope
   is **every subsequent command**, the two make the chain transitive across slices, submits and the
   consumer's own render pass. `TRANSFER` is in both scopes because the dump's copy is exactly the
   reader a dependency naming only the sampling consumer would have left out. Not `BY_REGION`: a
   conv draw reads a 3×3 neighbourhood and OUT reads a different image entirely, so neither read is
   framebuffer-local.
2. **`dst_ready` transitioned the destination from `UNDEFINED` unconditionally, including on a
   repaint** — which licenses the driver to discard every texel of the atlas the repaint exists to
   recolour *in place*. It would have blanked exactly the world it was added to avoid blanking. The
   job now carries `dstHas` and picks `SHADER_READ_ONLY_OPTIMAL` when there is something to keep.

#### What the landing review found, and it was all lifetimes

Four findings, every one verified against the code before acting and every one real. Three are the
same omission in different places: **the retire discipline was applied to the activations and to
nothing else.**

1. **`job_free` destroyed a live job's device objects outright.** It called
   `vkDestroyFramebuffer` on `dstFb`, destroyed the palette image and view, and
   `vkFreeDescriptorSets` on all five sets — with no fence and no retire. Three of its four callers
   are in a consumer's `prepare`, where the seam has waited on **this slot's fence and no other**,
   so the remaining `slots − 1` submits are still queued naming exactly those objects: the OUT
   render pass named `dstFb`, its set was `setOut[i]`, FILL sampled `palView`. A map change, a GL
   reset or a repaint within `slots − 1` frames of a draw is the whole trigger
   (VUID-vkDestroyFramebuffer-framebuffer-00892, VUID-vkFreeDescriptorSets-pDescriptorSets-00309) —
   a crash on a strict driver, corrupt paint on a lax one. The activations got a whole `RETIRE` for
   this hazard; the job's own resources got none, which is what made it easy to miss. They have one
   now, **sized at the worst case its callers can produce** (`TAGPU_R_MAXJOBS × TAGPU_VK_SLOTS`
   entries, under 4 KB) so the full case is unreachable rather than unlikely — and still handled,
   by draining the device, because a stall is honest and an unlicensed destroy is not.

   **Which trigger is actually reachable today is worth being exact about, and it is narrower than
   the finding says.** A *map change* is not one: §2.16's own measurement is that an in-process map
   change brings the whole Vulkan lane down and back up, so `tagpu_vk_terr_down` frees the job on
   the drained teardown path before any new `prepare` runs. The reachable trigger is the
   **repaint** — `glsl_begin(ta, 1)` on a palette move bumps the serial with the lane still up and
   the job still live — and that path is itself unexercised (below). So this was a real
   use-after-free whose only live trigger is one no fixture produces. **That is not a reason to
   have left it**: the teardown-first ordering is a coincidence of two modules' behaviour, which is
   precisely the "it works because of when things happen" argument `CLAUDE.md` refuses. The fix is
   a lifetime and does not care which path frees the job.
2. **Neither retire was in the teardown.** `down()` destroyed the live activations and flushed the
   framebuffer cache but never touched `s_ret`, so a retire outstanding at teardown — an activation
   grow, or the idle release after 180 quiet slices — leaked two array images, their memory, every
   layer view and up to 32 framebuffers, around **100 MB**, and `vkDestroyDevice` then ran with all
   of it alive. The resize path was accidentally fine (the device survives it and a later
   `retire_slot_done` cleaned up); `vk_down` had nothing that ever would. `lost()` now forgets both
   retires, because a `pending` left standing would have a rebuilt device destroy dead handles.
3. **A job captures `s_atlas.view` at creation and never re-reads it**, so any resize of the
   indexed atlas left its descriptors naming `s_atlas.oldView`, which `shared_slot_done` destroys
   `slots` frames later while the restorer draws on. `restore_want` normally frees the job in the
   same frame, but it sits *below* the resize and every `return 0` and `goto refuse` between the two
   skips it. The drop is now keyed on the view itself, immediately after the resize, where nothing
   can return first. (Reported as PLAUSIBLE; confirmed by reading the early returns.)
4. **The "mutually exclusive by construction" claim above was false**, and this section said it. The
   lever is *polled* until it latches, so it can be created mid-session — and then
   `rgb_mirror_step`'s early return stops updating the mirror while leaving `rows > 0` standing, so
   the publisher ran **both** blocks. A consumer resized its restored image twice in one call and
   uploaded a frozen mirror into the image the other lane renders into; where the mirror's rows and
   the atlas's height disagreed it refused every frame instead and **terrain stopped drawing
   altogether**. Fixed in two places on purpose: the latch frees the mirror (the memory is what the
   lever exists to retire) and the publish is an either/or (so the hand-over's own claim does not
   depend on a lever's latch order). Found independently by this session the same hour, which is
   worth recording only because it is the one finding the review did not have to catch.

**What the review verified clean** and is therefore worth not re-deriving: no SPIR-V word changed in
the eleven pre-existing headers (only the `transform` hash and the new comment lines); `rp_deps`
covers the whole chain including the dump's copy in both directions; `dst_ready`'s `dstHas`/
`clearDue` layouts are right on first job, repaint, `job_clear` and post-`down`; the weight offset
is a multiple of 256 given `offset & 15` and `kstride & 15`; `form_batch`'s two passes use an
identical predicate; `fb_for`, `s_rpAct[]`, `s_pipeConv[]`, `conv_spv` and the class indices are in
bounds; and `tagpu_restoreglsl.h` has no includes and no GL types, so `tagpu_terr.h` including it
drags nothing into a `_vk` translation unit.

**`tagpu_vk_restore_lost` still has no caller** outside `down` — the Vulkan seam has no
device-loss path, unlike the GL lane. It is documented, dead, and kept deliberately; the day the
seam grows one it is what that path needs.

#### Where the slice sits in the frame, and what it is not counted in

`tagpu_vk_restore_step` is called **last of the seam's `prepare`**, after every consumer. That is
the reverse of the shadow map's ordering and the same argument: the shadow map must run *before* the
passes that sample it, and the restorer must run *after* the passes that **feed** it, because a
consumer hands it a job and a frame list in its own `prepare`. Its own render passes are begun
inside `prepare`, which is legal there and nowhere else because render passes may not nest.

**It is deliberately not added to `ndraw` or `nclaim`**, for the reason the shadow pass is not:
those count the passes that put pixels in *this* frame, and counting one that puts none would refuse
every A/B capture taken with Classic++ on — which is exactly the configuration restored art is
measured in.

#### What this does NOT cover, stated rather than implied

* **One consumer of four.** The features, effects and unit atlases still take the CPU mirror. Their
  wiring is the same shape (one usage flag plus a published frame list) and is not done.
* **`repaint` is built and not exercised, and it takes two fixes down with it.** The
  palette-moved-under-a-live-job path needs a fixture that moves the palette; `static-terrain` does
  not, and an in-process map change tears the lane down instead of repainting. So `dstHas` is
  argued from the spec's discard rule rather than measured, **and the job retire's licence is
  unexercised for the same reason** — every path that frees a job while submits are in flight is a
  repaint. Both are lifetimes rather than timings, which is what makes them defensible unmeasured;
  neither is *shown*.

  **AND THE FIXTURE WAS ATTEMPTED, TWICE, AND IS RECORDED AS A NEGATIVE RESULT** (2026-09-17)
  rather than left as a to-do, because what it found bounds the gap. Three mechanisms were tried
  against a settled `static-terrain` restore, each confirmed by a **peek** rather than by
  inference:

  | mechanism | what it should move | what happened |
  |---|---|---|
  | the in-game options screen (`tab`, then `escape`) | the presented palette | no change — the palette serial stayed 2 |
  | `+showranges`, a typed cheat the `ta-drive` note records as working, used here as the **control** | `main+0x391BF` | **stayed 0** |
  | `+gamma 13` (`0x4172B2`, `SetGamma(N × 0.1)`) | `main+0x37F08`, then the palette | **stayed 12**, through both `+gamma13` and `+gamma 13` |

  The control failing is what makes this conclusive: it is **not** `+gamma` being refused, it is
  the chat path. Delivery is not the problem either — `tagpu.log` carries
  `input: keys x char:+ char:s …` for every token, and `tagpu_shield.c:362` translates
  `WM_TAGPU_CHAR` into a real `WM_CHAR` for the engine. The likeliest cause, **unproven and marked
  as such**: the Enter that opens the chat line has to reach the engine's own key handling, and TA
  takes game keys through DirectInput rather than from the window, so a posted `WM_KEYDOWN` never
  opens the line and the `WM_CHAR`s that follow have nowhere to land.

  **What this does and does not license.** It does *not* mean the repaint is dead code — the engine
  has a gamma slider in its own UI, so a player reaches this path and both fixes have to be right.
  It does establish that **the palette does not move on its own once a level is up**: three runs,
  two fixtures, the serial reaching 2 *before* the tile atlas is even built. So the next attempt
  should stop trying to provoke it through injected chat and either drive the gamma slider as a
  gadget ([gui-gadgets](gui-gadgets.html) places it on the campaign screen) or reach `SetGamma`
  another way. That is still the next thing worth building for this lane, and it is now a smaller
  and better-aimed question than it was.
* **The `restorevk` lever is measured in one configuration only**: created before launch, terrain
  only, one map, no mid-session flip. The mid-session flip is now correct by construction (the
  publish is an either/or) but has not been run.
* **The `g_alloc` ring is a bound that a fast enough device can reach.** 256 blocks per slot per
  slice; a FILL takes one, a CONV one and an OUT two, so a `full`-model batch spends 15 and a slice
  of more than about **seventeen batches** exhausts it and fails the job. Not reached on the
  reference setup (7 draws a slice at a 12 ms budget) and not raised, because the correct number is
  not knowable from one device. **An earlier version of this bullet said the failure was *loud* and
  said "two per draw"; both were wrong** — the review disproved them. The draw returned 0 with no
  log line at all, so the consumer reported "the restore failed" and nothing anywhere said why,
  which is exactly the silence this section criticises the vertex bug for, on the one bound the
  section concedes is reachable. `g_alloc` now names the reason once per slice.
* **One GPU, one model, one fixture, one map.** `tiny`, `fp16`, and any other device's limits are
  unmeasured on this lane.
* **The failure paths** — `act_ensure` returning 0, a destination that is not a complete render
  target, the out-of-memory returns — remain unreachable on a working device and unexercised, as
  §2.42 already records.

### 2.44 Two more consumers, and the parameter tables had the vertex bug too — landing 7d of the Vulkan-only plan

Landing 7c left three of the restorer's four consumers on the CPU mirror — four being the list the plan names; the UI atlas is a fifth and is not on it. This one wires the two
sprite atlases — **features** and **effects** — and the oracle it needed found a bug in the code
7c had already landed, in the sibling of the resource 7c fixed.

**What crosses the hand-over is different from terrain's, because the queue is different.** The
terrain's restore is a fixed list published whole under one serial: 10 036 tiles, arrived at once,
and a consumer that misses a frame re-reads the whole list on the next one. A GAF atlas is a
**lazy queue** — `tagpu_gaf.c`'s `restore_enqueue` adds one frame per miss for the life of the
atlas — so what is published is an **append-only list with a generation**, and the consumer holds
a **cursor** into it:

| | terrain (landing 7c) | features / effects (7d) |
|---|---|---|
| shape | the whole list, per serial | append-only, per generation |
| consumer state | the serial it built from | the generation **and a cursor** |
| a frame the consumer skipped | re-read next frame | still there; it takes more next frame |
| what restarts it | the serial moved | the generation moved |
| bound | the map's tile count | **four times the atlas's entry ceiling** |

`rlistGen` is the only thing a cursor cannot survive, and it is bumped by every event that makes
the array stop being a continuation: the arm, a recycle, a repack (both of which drop the GL
queue and blank the twin), a GL context loss, a palette move, and the overflow restart. A
consumer that sees a new generation drops its job and starts at index 0. `rlistRepaint` is 1 for
the palette-move generation alone, where the destination keeps what it holds.

**The bound is a restart, not a bigger buffer.** An append-only list fed for a session's length is
not bounded by anything; what *is* bounded is "the entries the atlas holds", so reaching the cap
re-seeds the list from those (a new generation, repaint 0) and the other lane blanks and repaints.
The arm path and the overflow recovery are **the same function** on purpose — it means the rare
path is the one exercised on the first frame of every session.

**Arming it frees the read-back's 16 MB**, and the publish is an either/or rather than a claim of
exclusivity — the lever is a poll that can land on any frame, which is exactly what made the same
claim wrong on landing 7c. `tagpu_gaf_atlas_mirror_rgb` also refuses while the list is armed, so
the two can never both be live.

**The restored twin's image now carries `COLOR_ATTACHMENT`** (`mk_image` takes the usage it is
for, in both passes), and **the restored refusal cannot always be a `return`.** This lane's own
restore needs the frame's indexed atlas uploaded before it can paint anything, and that upload is
below the refusal: returning on the frames before the first paint is a **deadlock**, not a
stand-down — nothing drawn because nothing painted, nothing painted because the atlas never
arrived. Those frames now run the uploads and stop without claiming the frame.

#### The bug: the parameter tables were staged per FRAME SLOT

`upload_tables` wrote each batch's three per-frame tables — destination rect, source rect, **key
and wrap** — into a host-mapped region indexed by the frame slot, under a comment that said *"one
batch is ever in flight, so nothing else writes them in between"*. That is true of the **device**
and false of the **recording**: a slice can issue more than one batch, which is landing 7c's own
finding, and both batches' `memcpy` landed on the same address before either
`vkCmdCopyBufferToImage` had executed. **The first batch's frames were therefore restored through
the second batch's tables** — wrong source rects, and wrong colour keys.

**The images themselves were already correctly ordered**: the write-after-read barrier in front of
each batch's copy names the previous batch's shader reads. That is exactly why the bug survived
7c — reasoning about the *images* finds nothing wrong, and the hole is in the staging buffer.

**What it looked like, which is why a sprite atlas was needed to see it at all.** A frame whose
key came from another frame's table has no keyed texel where it should have one, so the OUT pass
writes the key's own palette colour — opaque **(84, 84, 252)** — where the GL twin writes
`(0, 0, 0, 0)`. Measured on the first run of the sprite oracle, `fx-mix` at 1024×768:

| atlas | frames | entries differing | texels differing | the difference |
|---|---|---|---|---|
| `terr` | 5 062 tiles | — | **0** | identical |
| `feat` | 915 painted | **118 of 1 304** | 68 411 of 4 194 304 (1.63 %) | 13 803 of them opaque key colour against `(0,0,0,0)`; the rest wrong source rects |
| `fx` | 139 painted | **3 of 167** | 2 891 (0.07 %) | colour deltas to 112, alpha equal |

Every differing texel lay inside an entry's own cell, and 1 186 of 1 304 feature frames were
byte-identical — the signature of a per-batch parameter swap rather than of a wrong pass.

**The terrain could not have revealed this.** 10 036 tiles of one size with **no colour key at
all**, so a swapped table costs a source rect and nothing else; and the two runs that measured it
gave each slice one batch. It took a consumer whose frames have different sizes *and* a key —
which is the argument for wiring consumers rather than declaring the port done.

The fix is the same shape as 7c's and for the same stated reason: `vkCmdUpdateBuffer` puts each
batch's tables **in the command stream at the point of its own copy**, with a buffer barrier both
ways (TRANSFER → TRANSFER, because the previous batch's read of these bytes is a copy and not a
draw). A bigger arena is still not the fix — batches per slice is a time budget, not a count. The
staging buffer became device-local and unmapped, like the vertex buffer beside it.

**Re-measured, and with the condition present.** `fx-mix` again: all three atlases byte-identical,
**62 slices issued more than one batch** on the two sprite jobs while it ran, which is the
collision exercised rather than avoided. Then `static-terrain`, the fixture landing 7c measured on:
terrain byte-identical at **46 461 952 bytes**, features and effects identical beside it.

| run | fixture | `terr` | `feat` | `fx` |
|---|---|---|---|---|
| 1 | `feat-forest`, wrong arm set | identical | **no atlas at all** | **no atlas at all** |
| 2 | `fx-mix`, before the fix | identical | 118 of 1 304 frames differ | 3 of 167 differ |
| 3 | `fx-mix`, after | identical | **identical** | **identical** |
| 4 | `static-terrain`, after | **identical** (46 MB) | identical | identical |
| 5 | `fx-mix`, after the review's fixes | identical | identical | identical |

**And the terrain's clean result in landing 7c was never in danger, which took the landing review
to see.** The paragraph here first recorded it as an unexplained survival — 7c's re-measurement was
taken with batches 157 and 158 in one slice, so the swap should have cost batch 157 its cells — and
that reading was wrong about what the log line says. **`batch %d … issued at slice %u` is printed
in the OUT branch** (`tagpu_restore_core.c:474`, after `j->rbatches++`), so it timestamps a batch's
LAST draw. The tables are uploaded at the **FILL** (`tagpu_vk_restore.c`, the `TAGPU_RDRAW_FILL`
branch, its only call site), and the budget check sits between individual draws
(`tagpu_restore_core.c:607`), so a batch's FILL is normally several slices before its OUT:

| | what the log line shows | what the resource needs |
|---|---|---|
| the OUT vertices (7c's bug) | two batches' **OUTs** in one slice | exactly that — the vertices are written at OUT |
| the parameter tables (7d's bug) | nothing | two batches' **FILLs** in one slice |

So in 7c's run there was **no table collision at all** — batch 157 was a full 64-frame batch
(`cols` 8, so `TW = TH = 8S` and tens of slices of fragments) while batch 158 had 6 frames
(`cols` 3, about a seventh of the cost, cheap enough for its whole FILL→CONV→OUT chain to fit in
what was left of the budget). Nothing survived the swap; the condition was never present. **And
that is a stronger version of this section's own argument**: the sprite atlases' batches are small
and numerous, so two whole batches — two FILLs — land in one slice routinely (the 62 two-batch
slices measured here), while the terrain's big batches put at most one FILL in a slice. A clean
terrain run says nothing about this hazard either way, and the recipe in
[ta-drive](ta-drive.html) was telling the next operator to look at the wrong log line.

#### What else this landing found

**The sweep the fix asked for, since this is the second time.** Every other place in
`tagpu_vk_restore.c` that writes device-visible bytes at RECORD time, checked rather than assumed:

| what | when it is written | why it is safe |
|---|---|---|
| the globals ring (`g_alloc`) | once per draw | a **ring**: every allocation in a slice has its own offset, and the slot's fence is what licenses re-use across slices |
| the OUT vertices | once per batch | in the command stream since landing 7c |
| the three parameter tables | once per batch | in the command stream since this landing |
| the palette staging (`upload_pal`) | per job, when `palDue` | per **job**, not per slot, and a second write before the first copy executes leaves the image holding the NEWER palette — a batch can sample a palette one frame early, which is a staleness and not a swap |
| the dump staging | never written by the CPU | a copy DESTINATION, read after the slot's fence |

**And two of the twelve interface entry points have no callers at all** — `job_repalette` and
`job_clear`. Both consumers answer a palette move and a recycle by **rebuilding the job** on the
new serial or generation, which is what makes `repaint` reachable at all, so neither entry point
has ever run. That matters for the next consumer rather than for this one: `job_repalette` sets
`palDue = 2`, whose barrier names `SHADER_READ_ONLY_OPTIMAL` as the palette image's current
layout, and a caller that repalettes a job **before its first FILL** would transition from a
layout the image is not in. Harmless in effect (the copy overwrites the whole image) and a spec
violation regardless. Left as it is, named here, because inventing a caller to exercise it is not
this landing's work.

* **A bug in its own new code, found by reading the diff before the review.** `rlist_restart`
  seeded the list from the atlas's entries **bounded by the current allocation**, so an atlas
  holding more entries than the 256 the arm allocates would have lost the tail of its own list —
  silently, which on the other lane is cells that stay indexed for ever with nothing in the log.
  Both the seed and the append now go through one `rlist_room` helper that grows to what is asked
  for or drops the list and says so.
* **The arm set is part of the instrument.** The first run of the sprite oracle measured nothing
  at all: both passes logged `atlas=0` and no restore, because without `native.on=all wrecks` the
  feature pass never owns the leaf, emits nothing and atlases nothing. `feat.on` alone is not
  enough to make a feature atlas exist.
* **The oracle now belongs to the restorer.** The per-job dump moved out of `tagpu_vk_terr.c`
  into `tagpu_vk_restore.c`, so every consumer gets it for free — which is what the next one
  would otherwise have copied. Names are uniform: `tagpu_restore_<tag>.rgba` from the GL lane
  against `tagpu_restore_<tag>_vk.rgba` from this one, and the terrain's GL dump was tagged to
  match. Re-armed on the **paint count** rather than on a serial, because a lazy queue keeps
  painting and a dump is owed again whenever the picture has moved.

#### What the landing review found, and it was all lifetimes and one log line

One HIGH, three MEDIUM, six LOW, all verified against the code before anything was changed, and
**all of them on the lever path or in the notes** — nothing it found can reach a player, because
the shipped path is the read-back and the lever is absent by default.

* **HIGH — the dump's staging was destroyed outside the retire.** `job_free` freed it on the
  argument that it is *"called from a consumer between frames"*, which is not the same as being
  past the fence of the slot that recorded the copy: the window is up to `slots - 1` frames wide,
  and a generation change on any of them destroys a buffer a submitted command buffer still names.
  It goes into the `JRETIRE` that already exists for exactly this, beside the palette staging.
  (Found independently while briefing the reviewer, and reported by it too.)
* **MEDIUM — `rlistRepaint` was a property of the latest GENERATION, and "you must blank" is a
  property of the INTERVAL.** Two resets between two of a consumer's looks collapse into one, and
  `tagpu_feat.c` produces exactly that pair in a single frame: it recycles a full atlas (blank) and
  calls `tagpu_gaf_atlas_restore` on the next line, which repaints if the palette moved. The
  consumer would have been told to keep a destination the GL lane had just cleared. Fixed with a
  monotone **blank counter** published beside the flag; no sequence of generations can hide a
  blank from a consumer that compares it.
* **MEDIUM — "the read-back is then the fallback again" was false.** Both arm latches are one-way
  and arming the list frees the mirror, so after the list's out-of-memory drop there is neither —
  and the consumer went on drawing a frozen twin while the GL lane kept restoring. The comment is
  corrected and the stand-down is made real: the consumer clears its "this twin is a picture" flag
  when the request disappears under a live job.
* **MEDIUM — the same flag survived two permanent abandonments** (the job failing, and a new
  generation arriving after the device had refused), so the pass would sample the old layout's
  twin at the new layout's rects. *"What it painted stands"* is only true until the rects move.
* **LOW ×6**: a `%%s` that printed itself in the one line saying what the lane is waiting for; a
  cursor that advanced by frames *accepted* rather than *offered*, so a frame the restorer refuses
  permanently was re-offered every frame for the life of the atlas; an overflow restart that
  appended the frame it had just re-seeded; a generation-event list that was six events long when
  the code has seven; a stale `tagpu_gaf.c:689` citation this landing's own insertion invalidated;
  and a `cmp` loop in the skill that invites the reader to treat the unit atlas's missing Vulkan
  half as a failure.
* **And it explained the terrain paragraph above**, which is the finding that changed a note rather
  than a line of code.

#### What this does NOT cover

* **The units, which are the fourth consumer and the one with a seam of its own**: the unit
  atlas's twin is **mipped and trilinear**, and its mirror is the whole chain because GL's own
  levels are what make the two lanes byte-identical (§2.43's landing and the gate-3a pass before it). A Vulkan restore paints level
  0 only, so that consumer needs its levels generated on this lane — and a blit chain is this
  fork guessing at `glGenerateMipmap`'s reduction. That is the next landing's question, not this
  one's.
* **The UI atlas** (`tagpu_gui_surf.c`) is a fifth restore consumer and keeps its read-back; it
  has a `restoreMinEdge` floor and its own arm beat.
* **The mid-session lever flip** is correct by construction on both halves now and still has not
  been run.
* **`repaint` remains unexercised** on this lane, for the reason §2.43 records at length: no
  fixture has been found that moves the palette once a level is up.
* **The effects atlas churns during a fight**, and the two lanes' dumps are taken on each lane's
  own "idle": a scene that is still adding frames can therefore be compared at two different
  moments. The comparison above was taken after the fight settled, and that is a property of the
  measurement rather than of the code.
* **The failure paths the review's fixes are about are still unexercised.** The blank counter, the
  cleared "this twin is a picture" flag on an abandoned restore, the out-of-memory list drop and
  the dump's retire were all reasoned to and none of them has been run: the re-measurement after
  the fixes (run 5 above, **91** two-batch slices) proves the steady path still produces the GL
  picture byte for byte, and proves nothing about the paths themselves.
* One GPU, one model (`full`), one map, one fixture per atlas.

### 2.45 What `glGenerateMipmap` actually did to the restored twin, and what that licenses — landing 7e's measurement

The units are the restorer's last unwired consumer and the only one with a seam of its own: their
twin is **mipped and trilinear** (`ATLAS_MIP 2`, `ATLAS_PAD 4`, 2048 square), a Vulkan restore
paints level 0, and the levels have to come from somewhere. `tagpu_gaf.h` has said since gate 3a
that *"a blit chain on the Vulkan side would be this fork guessing at `glGenerateMipmap`'s
reduction, and the guess would be a per-driver difference that no note could pin down"* — so
before writing that landing, **the driver was asked what it actually did**, which is a question
this repository can answer rather than assume.

`tagpu_gaf.c`'s dump now writes the whole chain (`tagpu_restore_unit.mips`, 3 levels of 2048,
21 MB, under the same `tagpu_restoredump.on`), and each level was held against six candidate
reductions of the level above it. `crowd-static`, 155 entries, 101 488 texels of level 1 carrying
art:

| candidate | level 1 exact | level 2 exact | max &#124;Δ&#124; R,G,B,A |
|---|---|---|---|
| **`(sum + 1) / 4`** | **99.61 %** | **99.63 %** | **1, 1, 1, 0** |
| `floor(sum / 4)` | 95.96 % | 96.00 % | 1, 1, 1, 0 |
| round-half-to-even | 96.92 % | 96.97 % | 1, 1, 1, 1 |
| two-pass h then v, round up | 91.78 % | 91.86 % | 1, 1, 1, 1 |
| alpha-weighted (premultiplied) | 96.92 % | 96.97 % | **57, 56, 49, 1** |
| sRGB-aware (linearise, average, re-encode) | 93.40 % | 93.10 % | **54, 45, 45, 1** |

**Two of the six are ruled OUT by the maximum rather than by the average**, which is the reason to
report a maximum at all: alpha-weighting and gamma-awareness are wrong by up to 57 and 54 levels on
a single channel while still matching most texels exactly — 96.9 % for alpha-weighting, 93.4 % for
the sRGB-aware one. An average alone would have called them plausible. (An earlier version of this
paragraph said "97 %" of both; it is 97 % of one of them, and landing 7e-1's review caught the
roadmap repeating the rounder number.)

**What the driver does is an unweighted 2×2 box average of RGBA, with a rounding rule none of the
six reproduces exactly and all of them reproduce to within one level.** Two further facts pin it
down:

* **Alpha is exact** (max Δ 0) under `floor`, `(sum+1)/4` and the two-pass floor — so alpha is
  reduced the same unweighted way, and the disagreement is purely RGB rounding.
* **The differences are on FULLY OPAQUE quads**, not on the mixed-alpha edges: of `floor`'s 42 371
  differing texels at level 1, **42 370 sit on a quad whose four alphas are all 255** and exactly
  one sits on a mixed quad. A different *filter* would have shown up at the edges; a different
  *rounding* shows up in the interior, which is what this is.

Chasing the last 0.4 % to an exact formula was not done: the useful result is the **bound**, and
the bound is **±1 per RGB channel and 0 on alpha**.

**What it licenses, and the choice it puts in front of landing 7e.** Three options, and the
measurement is what makes them comparable rather than a matter of taste:

| | what the Vulkan lane does | what the oracle can claim | what it costs |
|---|---|---|---|
| **A** | keep reading GL's levels back (today's mirror) | `cmp`, byte-identical | keeps the one read-back this plan exists to retire, and it would be the last one |
| **B** | reduce with our own box average | **within 1 LSB on RGB, exact on alpha** — a measured bound, not equality | the plan's strongest instrument weakens from `cmp` to a bounded diff |
| **C** | reduce with our own box average **on both lanes** — `glGenerateMipmap` replaced by the same pass on the GL side | `cmp`, byte-identical **by construction on every driver** | changes the shipped GL twin's minified texels by ≤1/255 per channel |

**C is the recommendation**, and the measurement is what makes it a small decision: the GL side's
own levels already differ from a box average by at most one level, so replacing the driver's
reduction with ours moves no texel by more than 1/255 in art that is being minified — while
removing a **per-driver unknown** from the shipped path and keeping `cmp` as the oracle for all
four consumers. B is the fallback if the GL picture is to stay untouched; A is what the plan would
have done by default, and it is the only one that leaves a read-back behind.

**Not covered:** one driver, one atlas, one fixture. The reduction a *different* GPU performs is
exactly what option C stops mattering and what options A and B leave open — which is the argument
for C stated as a property rather than as a preference.

### 2.46 The twin's levels stop being a driver's rounding rule — landing 7e-1 of the Vulkan-only plan

§2.45 measured what `glGenerateMipmap` does to the restored unit twin and put three options in
front of this landing. **Option C landed, and this is its GL half**: the twin's levels 1.. are
reduced by a pass of ours rather than by the driver, so that the levels the GL lane samples and the
levels a second backend will paint are the same bytes *by arithmetic* instead of by driver luck.

**The pass.** `TAGPU_RESTORE_MIP_FS` (`tagpu_restore_glsl.h`) is the exact integer 2×2 box average
— four fetches, `round(v * 255)` per channel, `(sum + 1) / 4` in integers, written back through the
same `(k + 0.25)/255` trick the OUT pass uses so that a driver which truncates the float-to-unorm
conversion and one which rounds both store exactly `k`. `tagpu_rglsl_mips(tex, dim, mip)`
(`tagpu_restoreglsl.c`) draws it per level into its own framebuffer, and `tagpu_gaf.c`'s
`twin_mips` calls it with `glGenerateMipmap` as the fallback.

**Three things the caller owes the shader, and they are why the fetch is exact rather than nearly
exact:**

* `GL_TEXTURE_BASE_LEVEL` **and** `GL_TEXTURE_MAX_LEVEL` both set to the source level. There is no
  level argument in the shader because the destination is another level of the *same* texture:
  clamping the sampler to the one level being read is what makes rendering into level `L` while
  sampling level `L-1` legal rather than a feedback loop, and it needs no copy.
* the filter pinned to `GL_NEAREST` for the duration. With a non-mipmap minification filter only
  the base level is ever sampled, so the fetch cannot drift to a neighbouring level however the
  implementation computes its level of detail — and NEAREST returns the texel itself rather than a
  bilinear blend that merely happens to weight one texel 1.0.
* the source width in `uSrcDim` rather than `textureSize()`. Both would work; the uniform says in
  the C what the shader reads, and the dimension is a power of two here, so `1.0 / uSrcDim` and
  every texel centre are exact in float.

All four borrowed texture parameters are read back and restored, and the chain is refused whole —
falling back — rather than reduced wrongly if any level of it would be odd: GL's own rule for an
odd level is a weighted three-tap, not a 2×2 average.

**The measurement, on §2.45's own fixture and instrument** (`crowd-static`, 155 entries, the whole
chain dumped to `tagpu_restore_unit.mips` under `tagpu_restoredump.on`):

| level | texels | equal to `(sum + 1) / 4` of the level above | max &#124;Δ&#124; R,G,B,A |
|---|---|---|---|
| 1 (1024²) | 1 048 576 | **1 048 576 — 100.00 %** | **0, 0, 0, 0** |
| 2 (512²) | 262 144 | **262 144 — 100.00 %** | **0, 0, 0, 0** |

For contrast, **our** chain against four other reductions — these are *not* §2.45's figures and
are not comparable with them, because §2.45 measured candidates against the DRIVER's chain and
these measure them against ours: `floor(sum/4)` 96.24 %, `(sum+2)/4` 94.20 %, alpha-weighted
96.61 % (max Δ 57), sRGB-aware 93.23 % (max Δ 54). The driver's chain is no longer on disk — the
reduction replaced it — so §2.45's table is the record of it, and re-taking those figures would
mean standing the reduction down deliberately.

**The three sprite pairs stayed `IDENTICAL`** on the same run (terrain 23 674 880 bytes, features
and effects 16 777 216 each), so landing 7d's oracle did not move.

**What establishes that the reduction actually ran is the 100.00 %, not the absence of a log line**
— and the difference matters enough that the review made it a finding. The driver's own chain was
95.96 % / 96.00 % against the nearest candidate, so a run that fell back could not read 100.00 %;
whereas four of the six ways to stand down were *silent* when this was first written, so an empty
grep proved nothing. All six say so now, once per context and with the reason. Note also that only
**one** twin reduces at all: `twin_mips` returns before the reduction when `mip` is 0, and
`tagpu_render3do.c` is the only caller that sets it — the terrain, feature, effects and UI atlases
are unmipped, so "every twin" would be three twins doing nothing.

**And the unit pass's own A/B still reads 0 px** — `selbox-facings` at 1024×768 with
`classicpp.cfg=assets=1 shadows=1 terrainshadow=1`, **0 differing pixels of 786 432**, 2 125
non-black on each lane. That measurement is the concrete reason option C was taken rather than B:
reducing on the Vulkan side alone would have scattered ±1 through every minified unit texel and
turned this gate from an exact 0 into a permanent small residual.

**Be exact about what that 0 px does and does not prove.** The unit consumer on the Vulkan lane is
still fed by the **mirror** — the whole chain read back off this twin, levels and all — so the two
lanes sample the same bytes today because one copies them from the other, and this A/B would read 0
under either reduction. What it establishes is that changing the reduction did not move the unit
picture. The claim that the two lanes agree **without** the copy is 7e-2's to earn; what landed
here is the arithmetic that makes it earnable.

**A defect this found by reading rather than by measuring, and it was in the first version of the
change.** The twin was created with `glTexImage2D` for **level 0 only**; every other level existed
because `glGenerateMipmap` created it. A reduction draws *into* level `L` through a framebuffer,
and a level with no storage makes that framebuffer incomplete — so the first chain of every twin
would have failed the completeness check, fallen back silently, and a twin painted once and never
again would have kept the driver's chain **for good**. The fix is at creation: every level 0..`mip`
gets its own `glTexImage2D`. The levels are then undefined until the `twin_mips` that follows the
job's creation, and what makes that safe is an **invariant, not the shortness of the window** — the
first version of the comment argued the window ("exactly the two statements"), which was both wrong
(it is six statements including a call) and the shape of argument this project's rules reject.
The invariant is three facts: nothing on that path samples the twin, the only thing that touches it
being the job creation rendering into level 0 to clear it; the one path that abandons the twin
**deletes** it, so no twin with an undefined chain is ever published; and `twin_mips` cannot fail to
write the chain, because `tagpu_gaf.c` demotes `mip` to 0 before every creation if
`glGenerateMipmap` did not resolve, so the fallback is guaranteed to be there when it is needed. (Before this landing the same call was merely
*consistency*; now it is what makes the twin samplable at all. Those are different failures by the
GL specification rather than by measurement: a texture whose chain is incomplete samples as a
defined `(0,0,0,1)`, while a level allocated with `NULL` and not yet written holds whatever the
driver left there.)

**The build gate demanded the Vulkan shader before the Vulkan consumer.**
`tools/spirv-gen.py` refuses a shader in `tagpu_restore_glsl.h` that no program in its manifest
uses, so `restore_mip` is paired and generated here — one small array in
`inc/spirv/tagpu_restore_glsl.spv.h` that nothing `#include`s until landing 7e-2 wires it. That is
the gate doing its job rather than something routed around: the two lanes cannot drift apart while
one of them is unwritten. Adding the row re-hashed the `transform` line of all twelve generated
headers, which is the freshness chain covering the tool's own source.

**Re-measured after the review's fixes**, on the same fixture: the chain is **byte-identical to the
pre-fix run** (`cmp` of the two dumps), still 100.00 % at both levels, the three sprite pairs still
`IDENTICAL`, and **no stand-down line fires on the happy path** — which was the one way the logging
fix could itself have been a bug.

**And the pending-error report fired, once, which is the LOW finding paying for itself in the same
run.** `restoreglsl: 1 GL error(s) were pending before the mip reduction (not ours)` appears
immediately after `restoreglsl: activations 512x512 x 16 layers (128 MB)` — so something in that
neighbourhood leaves the process-wide flag set, and before this landing the reduction consumed it at
frame top with no line. It is **not the reduction's own**: an error raised by the reduction fails it
and prints the other line, which did not appear. The separate slice-bracket report
(`before slice 2`, at startup) is the pre-existing one and is unchanged. Whose it is has not been
chased — it is one line in the log now, and the line before it says where to look, which is the
whole point of not swallowing it.

**Not covered.**

* **The Vulkan lane does not reduce yet.** This landing makes the GL side an arithmetic fact; the
  claim that both lanes agree is 7e-2's to earn, with per-level views on the twin and the oracle
  extended to `cmp` whole chains rather than level 0. The unit consumer on the Vulkan lane is
  still unwired, which is why the unit pair does not appear above.
* **The fallback is still the driver's**, with §2.45's bound: ±1 per RGB channel and 0 on alpha, on
  the one driver that was measured. It is reached only when the reduction cannot build or a level
  is odd, and it says so once in the log.
* **Anisotropy is unchanged** and is still the one sampler difference the port cannot close
  (`aniso=` is read by both lanes for exactly that reason).

### 2.47 The unit twin's whole chain, and the record-time latch that hid an empty atlas — landing 7e-2

§2.46 made the GL lane's levels arithmetic. This landing makes the **Vulkan** lane paint the unit
twin end to end — level 0 through the OUT pass, levels 1..mip through the same integer
`(sum + 1) / 4` — and extends the oracle from a `cmp` of level 0 to a `cmp` of **whole chains**.

**Measured, three consecutive runs on `crowd-static` (2026-09-17):**

| pair | result |
|---|---|
| `terr` | IDENTICAL, 23 674 880 bytes |
| `feat` | IDENTICAL, 16 777 216 bytes |
| `fx` | IDENTICAL, 16 777 216 bytes |
| `unit` **chain** | **IDENTICAL, 22 020 096 bytes — level 0 and both mip levels** |

The unit twin is 155 frames over a 2048 square with `ATLAS_MIP 2`, so the chain is
2048² + 1024² + 512² texels and every one of them matches the GL lane's byte for byte.

**The defect this landing spent most of its length on, because the shape is the lesson.** The
Vulkan lane's first one or two unit batches came out **black** — every texel, alpha 1, eight
near-black values — while every later batch was byte-perfect. The cause was not in the restorer at
all:

* `tagpu_vk_unit_upload` reached `standdown` on its **feed** path — the frame that comes only to
  make the restore job and draws nothing — and `standdown` calls `slot_free`, which is
  `kill_buffer(&s->vstage)`. By that point `atlas_upload` had memcpy'd the indexed mirror into
  that buffer and recorded a `vkCmdCopyBufferToImage` out of it, and `cb` is submitted whether the
  pass draws or not. The source buffer of a pending copy was destroyed under it.
* **The damage is the latch, not the lost copy.** `atlas_upload` sets `s_atHave`, `s_atSerial` and
  `s_atRows` at **record** time, so after the lost copy the pass believes the device holds rows it
  never received, and it will not re-upload until the mirror's serial moves again. `restore_want`,
  three lines below the upload, then hands the restorer frames `covered_prefix` calls covered.
* The restore therefore ran against an **empty** atlas image. OUT computes `frag = c - net`, and
  with the source reading 0 both `c` and `net` come from palette index 0, so the cell is black.
* **Only the Vulkan lane can see it.** The GL lane samples `tex`, which was never missing the art.

Fixed by construction — a **lifetime**, not a timing: the feed path leaves through `feedout`, which
destroys nothing, exactly as `refuse` does and for the reason `refuse` already stated. Every other
`goto standdown` is above `slot_vstage`; the label's comment said "safe here and only here", which
reads as a property of the label rather than of its callers, and both comments now name the side of
the staging they are safe on.

**What the measurements ruled out first, recorded so nobody re-walks it.** Each of these was a
plausible reading of "the two lanes restored the same bytes differently" and each is wrong:

* the activation arrays' growth, the descriptor generations, the framebuffer cache and the retire —
  pinning the arrays to 512 square so they are created once and never re-created reproduces the
  failure **byte-identically**;
* the CPU mirror — per-frame non-zero counts taken at upload time give `empty=0` of 25 listed
  frames;
* the staging memcpy — the staged bytes are byte-for-byte the mirror, 55 608 == 55 608;
* the palette — it is set once, before the job exists, and never moves.

**What identified it** was making the OUT shader report the index it had read, on *both* lanes: only
the R channel differed, GL read the art and Vulkan read **0 everywhere**. Then reading the device
image back **early** showed 404 798 texels missing where the same read-back taken later is exact.

**The oracle line that hid it, and this is worth more than the fix.** `unit SOURCE: IDENTICAL`
compares the GL texture against the Vulkan device image **after everything has settled**, so it
cannot see a source that was empty while the lane was restoring. It reported "the two lanes restored
the SAME bytes differently" and the premise was false. A dependent-lane oracle must compare the
lanes' **inputs at the moment of use**, not at the end of the run; when a dependent lane's picture
is wrong, make the shader report its input before theorising about its arithmetic.


**The review's four code findings, all verified against the source and all acted on.** Three of
them are about paths this driver never takes, which is exactly why they needed a reader rather than
a run:

1. **A level-0-only twin drew no units at all, for the session.** `atlas_rgb_build` created the
   per-level views only for `mips >= 1`, but the OUT pass paints *through* `s_arLvl[0]`, so with
   `restoreMips == 0` the job was created with a null `dstView`; `job_new` refuses that **without a
   word**, `s_rjTried` is cleared only by a teardown, and the pass then stood down on every frame.
   `restoreMips == 0` is not hypothetical — `tagpu_gaf.c` demotes the atlas's `mip` to 0 whenever
   `glGenerateMipmap`/`glTexParameterf` do not resolve, and the build site admits `>= 0` by name. So
   the views start at level 0, the prerequisite gate is unconditional, and the refusal says so.
2. **The "twin moved under a live restore" guard could not fire in two of the cases it exists for**
   — it tested `s_arLvlN > 0`, and `atlas_rgb_build` leaves that 0 on precisely the paths that
   destroy the image and every view over it. It was also in the wrong place: it runs later in the
   frame than the destruction it guards against. The job is now dropped by `atlas_rgb_build` itself,
   before `kill_image`, so its framebuffers retire on the mask a submitted command buffer is bound
   by; the check in `restore_want` stays as a backstop and can now actually fire.
3. **The forced first mip reduction sampled level 0 while it was still `UNDEFINED`.** `chain_step`
   ran one pass over an unpainted twin, on the stated grounds that the levels begin undefined and
   the consumer samples trilinearly — and both halves were wrong: the consumer does not sample the
   chain until `painted > 0`, and level 0 is undefined too until `dst_ready` transitions it in the
   OUT path. A descriptor declaring `SHADER_READ_ONLY_OPTIMAL` over an `UNDEFINED` image is
   undefined behaviour, and it fired on the *ordinary* first frame.
4. **A degenerate list frame stalled the cursor for good.** `covered_prefix` returned at `h <= 0`
   as well as at "not yet uploaded", conflating a frame the restorer will refuse with a coverage
   boundary — so every frame behind it stayed unpainted while `s_arHave` still called the twin a
   picture. It answers the coverage question alone now, which is what makes `restore_want`'s own
   "a frame the core can never queue is skipped for good" true.

The chain re-measured `IDENTICAL` after the fixes, on both fixtures.

**Not covered.**

* **The same record-time latch is still there in `tagpu_vk_feat.c` and `tagpu_vk_fx.c`.** Their
  `atlas_upload` equivalents set their have/serial/rows the same way, and neither has a feed path
  reaching a slot-freeing stand-down today — so the hazard is unreachable in those two rather than
  absent, and their pairs are identical for that reason and not because the pattern is sound. Any
  new stand-down added below their staging allocation re-opens it.
* **`tagpu_vk_restore_job_repalette` still has no caller** while `tagpu_rglsl_job_repalette` is
  called from `tagpu_gaf.c`. The palette does not move in the measured fixtures, so the two lanes
  agree; the moment a player touches the gamma slider they will not.
* **`repaint`, the blank counter, the cleared-picture flag and the out-of-memory list drop remain
  unexercised**, as does the dump's retire. `job_clear` has no callers.
* **Anisotropy is unchanged** and is still the one sampler difference the port cannot close.

### 2.48 The lane becomes a backend, and its surface goes on the game window — landing 4a of the Vulkan-only plan

**What this part is, and what it is not.** `render_vk.c` gives the Vulkan lane a render thread of
its own — `vk_render_main`, dispatched from `dd.c` on `tolower(g_config.renderer[0]) == 'v'` — and
`tagpu_vk_own_present()` puts the surface on the window `tagpu_vk_frame` is handed instead of on a
window of route D's. It calls **no gather half**, so no pass has a hand-over and the frame is the
seam's clear colour and nothing else. That is the part's whole claim, and it is measurable on its
own: whether a swapchain lives on the game's own window *inside the game*, which nothing had run.

| what ran | result |
|---|---|
| `renderer=vulkan`, **no lever file present** | `up in 418 ms … our window 00020058 over 00020058, 640x480 … route E: the surface is on the game window and there is no GL lane` |
| an X grab of that game window | **307 200 of 307 200 px** at the lane's clear colour (255, 0, 255) |
| `renderer=openglcore` + `tagpu_vk.on` (the control) | `window: created 00010074 over 00030054 (route D)` — unchanged |
| that control's `tacli glshot` | **148 distinct colours** — the GL lane's picture is whole |

**100 % of the window being ONE colour is the correct result here and not a defect**, and this
note says so because §2.35's lesson is the opposite shape: *"0 px over content that is not there
is not a measurement"*. The reading that would have been wrong is a *partial* one — some of the
window the lane's clear and some of it stale desktop or the engine's own blit. What the two
handles being equal (`00020058 over 00020058`) adds is that it is the **game's** window rather
than one of ours, which is the only thing route D could not do.

**Route E is now measured in the game as well as in the probe**, and the difference matters
because this seam has already had an API say yes while the screen said no. `tools/vkcoexist.c`
route E (§G19a in [roadmap](roadmap.html)) answers it on a bare top-level window; this answers it
on TA's own window, with the fork's window management, the input shield and the engine's message
pump all in play.

**What the lane costs in the real process, which the probe understates by ten times.**

| | committed peak | largest free block |
|---|---|---|
| before bring-up | 36.1 MB | 255.5 MB |
| with Vulkan up | 59.5 MB | 255.5 MB |

So **23.4 MB**, against the **2.2 MB** the 320×240 clear-only probe reads. Both figures are
right about different questions and the probe's own caveat said which: it bounds the *bring-up*
footprint, not the running one. The invariant survives the move — **the largest free block does
not change**, in the game as in the probe, which is the number that matters in a 32-bit process
because TA fails by failing to allocate rather than by saying anything.

**Route D is unreachable rather than disabled, at two places, and the second one is the point.**
The `ST_OFF` branch never asks for a window when the present is owned, so `s_vkwnd` stays NULL
and every `vkw_*` path is already inert through its own `if (s_vkwnd)` guard. That is
unreachability *by inspection*, which is the kind of argument this lane has been wrong about
before — so `tagpu_vk_wndproc` returns early as well, and a `VKW_CREATE` that was already in the
queue when the latch was set cannot create one behind us. The latch is one-way: which backend the
process has is settled at `dd.c`'s dispatch and cannot change, and a flag that could go back
would permit a surface on the game window and a route D window at once.

**Three things the seam had to be told, each of which would have been a live bug:**

* **"Our window went away" is not a case when we have no window.** The rebuild trigger tested
  `s_vkwnd != s_vk.hwnd`; with the present owned, `s_vkwnd` is NULL for the process's life while
  `s_vk.hwnd` is the game window, so that test would have been **permanently true** — the lane
  tearing itself down and rebuilding on every single frame, for ever. The same class as the
  comment two lines below it, which records the earlier version comparing the caller's `w`/`h`
  against the swapchain extent and rebuilding every frame because the two are different numbers.
* **The lever has to retire, in both directions.** `tagpu_vk.on` must stop arming the lane
  because `renderer=vulkan` already did; `tagpu_vk.off` must stop *disarming* it, because with no
  GL lane behind it a disarmed Vulkan lane is a black window rather than a fallback. The ON file
  is still read for its `color=`, which every A/B on this plan uses.
* **A log line that claimed something about a lane that is not in the process.** The bring-up
  ended `- route D: the GL lane's window is untouched` unconditionally. It now names the route it
  came up on, which is also the only thing distinguishing `our window X over X` from a mistake.

**The GDI fallback is allowed to be late, and that is a measurement rather than a preference.**
`tagpu_vk_failed()` reads the lane's own `ST_FAILED`, published after every path the bring-up
could have succeeded on — a fact, not a frame count. Handing the session to `gdi_render_main`
after a surface has been attempted on the HWND is sound only because route F measured GDI as
still reaching the screen there; had it not, the fallback would have had to be taken *before*
`vkCreateWin32SurfaceKHR` and any later failure would have been terminal.

**`dd.c` gets the dispatch and nothing else, checked site by site rather than assumed.** Every
other `g_ddraw.renderer == ogl_render_main` test in that file is a WGL workaround a swapchain
must not inherit, and adding `|| renderer == vk_render_main` to any of them would be silent:

| site | what it does | why Vulkan must not have it |
|---|---|---|
| `dd.c:850`, `:995` | `nonexclusive = TRUE` | stops WGL taking fullscreen exclusive; its one consumer is `:1120`, so GL-only here means exactly "no extra scanline" |
| `dd.c:1120` | `render.height++`, `opengl_y_align = 1` | a scanline added so the driver cannot take exclusive mode, plus the viewport shift paying for it. `opengl_y_align` is read in `render_ogl.c` and nowhere else — a Vulkan frame inheriting it would be one pixel too tall and offset |
| `dd.c:1265`, `:1354` | `ogl_create()`, GDI on failure | the Vulkan bring-up is the render thread's own |
| `dd.c:1525` | `SetPixelFormat` | already GL-gated, and route E is measured on a window without one. The HDC beside it is taken unconditionally, which is what leaves the GDI fallback a valid one |
| `dd.c:1788` | `ogl_release()` | nothing to release, and `dd.c` joins the render thread before reaching it, so `tagpu_vk_render_stop` has already run |
| `fps_limiter.c:153` | `fpsl_dwm_flush()` instead of the vblank wait alone | **outside `dd.c`, and on this backend's per-frame path.** A Windows-7 DWM workaround; `!IsWine()` makes it inert on the reference setup, and the Vulkan present does its own pacing |
| `winapi_hooks.c:2039` | `ogl_release()` in `fake_DestroyWindow` | nothing to release — and that function is the **only** writer that nulls `g_ddraw.hwnd`, which is why the `s_ownGone` latch exists rather than trusting `hwnd != s_owner` |

**The line numbers above are post-change, and the first version of this table had every one of
them off by one** — adding `#include "render_vk.h"` at `dd.c:12` shifted the file under an audit
recorded against the pre-change numbers, so four of the seven cited a `}` or a blank line. Caught
by the landing review. The scope was wrong too: the audit said "in `dd.c`" while one of these
sites is in `fps_limiter.c` and runs every frame.

**Not covered by landing 4a:**

* **No gather half runs**, so no pass draws — that is 4b, and it is where the per-pass previous-build
  A/B against `0b5e06d` gets taken. `tagpu_overlay_draw` is the single driver and most of it is
  API-independent; the GL-owning entry points are `tagpu_scaffold_frame`, `tagpu_native_frame`,
  `tagpu_gui_present` and `tagpu_fps_present`.
* **`TAGPU_FRAME` is not filled**, and 4b must fill it *before* calling `tagpu_vk_frame` with the
  same frame number — a pass refuses a hand-over stamped with any other frame. Note `vp_y` takes
  `viewport.y` **without** `opengl_y_align` on this path.
* **`ss` has no target and TA's surface is not uploaded by the backend** — 4c.
* **Route D's window, `tagpu_vk_wndproc`, `WM_TAGPU_VK` and the geometry tracking still exist** —
  4d deletes them, and only after 4b's figures are banked.
* **The GPU row cannot retry after the GDI fallback**, and this is a real loss rather than an
  oversight. While the lane was a lever beside GL, a failed bring-up retried when the player
  picked another device (`lane_gen() != s_choiceSeen`). Once the session is on GDI the thread is
  GDI's for the process's life. The alternative — sitting in the loop presenting nothing while the
  player hunts for a device that works — leaves them with no picture at all.
* **One launch, one GPU, one driver.** Every figure above is the linux NVIDIA ICD under system
  wine at 640×480 in the shell. Nothing here speaks for Windows, for llvmpipe, or for a frame with
  a world in it.

**Three residuals the landing review named and this landing deliberately did NOT fix**, each with
the reason, because shipping a fix that cannot be measured is not a landing here:

* **`WM_ERASEBKGND` is not claimed on the game window.** Route D's own window proc returned 1 for
  it — *"we paint every pixel from the present; GDI must not flash over it"* — and
  `g_ddraw.hwnd` has no such protection: the fork's `fake_WndProc` only sets `clear_screen` and
  falls through, and `dd.c:1445` issues a `RDW_ERASE | RDW_INVALIDATE` on every mode set. So an
  erase can paint over the swapchain's last presented image until the next present. **The GL
  backend has the same exposure**, so this is inherited rather than introduced — and 4a's frame is
  a flat clear, so a flash is not observable in it: an X grab of 307 200 identical pixels is
  consistent with a window that flashes. **It belongs to 4c**, where the backend owns TA's surface
  and there is content for a flash to interrupt, which is the first point at which the fix can be
  shown to work.
* **The GDI fallback's on-screen banner blames OpenGL.** `render_gdi.c` prints *"please update your
  graphics card driver (%s)"* with `g_oglu_version`, which is the zero-initialised array on this
  path because `dd.c` deliberately never calls `oglu_load_dll()` for `renderer=vulkan` — so it
  reads *"…driver ()"* and names the wrong subsystem. Safe, and the `TRACE` beside it is correct.
  Rewording a shared banner belongs with landing 11, which deletes GL and makes the OpenGL wording
  wrong everywhere rather than only here.
* **The zero-extent bring-up retry is now unconditional.** `up_worker`'s "the window has no extent
  yet" path returns the lane to ST_OFF and the next frame tries again; with the lever gone that
  spin is no longer gated on arming, so a span with a zero client rect costs one instance and
  device creation per ~420 ms for as long as it lasts. **Left as it is on purpose**: the retry is
  what gets the lane up once the window has an extent, and a bound that stopped retrying would
  wedge the lane for the session — a worse failure than the waste.

### 2.49 The gather halves run with no context, and the A/B learns to arm itself — landing 4b-1 of the Vulkan-only plan

**What this part is.** 4a's backend called no gather half, so no pass had a hand-over and the
frame was the seam's clear colour (§2.48). This part wires `tagpu_overlay_draw` into
`vk_render_main` and then teaches two of the driver's four GL-owning entry points to run their
gather and stand their draw down: `tagpu_scaffold_frame` and `tagpu_fps_present`. It also fixes
the thing that made *any* of this unmeasurable — the Vulkan capture was armed by the GL capture
having reached the disk.

**The driver first, and one frame number for the whole frame.** `vk_render_main` fills a
`TAGPU_FRAME` and calls `tagpu_overlay_draw` before `tagpu_vk_frame`, so everything the fork adds
that is not a GL draw is live under `renderer=vulkan`: the input injection, the trigger readers,
the own-draw flushes, the palette, the zoom, the packet exchange and the reclaim bracket. Two
details in it would each have been a silent bug:

* **The frame counter is taken once, before anything uses it.** A post-increment inside either
  the driver call or the present call would have differed by one, and a Vulkan pass refuses a
  hand-over stamped with any other frame — so the symptom would have been every pass standing
  down, not an error.
* **`vp_y` takes `render.viewport.y` WITHOUT `opengl_y_align`.** That is the extra scanline WGL is
  given so the driver cannot take exclusive mode (`dd.c`, and §2.48's audit); carrying it would
  offset every Vulkan frame by one pixel, and no capture would catch it because both halves of a
  two-lane A/B would carry it equally.

`surface_tex` is 0: there is no GL texture to name, and 0 is what the fork already hands a
non-8bpp frame, so it is in-contract rather than a sentinel. Its consumers have nothing until
4c takes over TA's surface upload.

**AND THE COUNTER HAS TO OUTLIVE THE THREAD, which the first version did not.** It was a local,
so it restarted at 0 — and `dd_SetDisplayMode` joins the render thread (`dd.c:747`, INFINITE) and
`dd.c:1459` creates a new one, so every mode change replayed the whole sequence of frame numbers.
The GL lane's `g_tagpu_frames` is a file static (`render_ogl.c:47`) and never repeats. This
matters because **every hand-over in the tree tests freshness by exact equality on that number**
and says so in as many words — `tagpu_terr.c`'s *"THE STAMP IS WHAT MAKES THE POINTERS ABOVE
SAFE"*, and the same test in `tagpu_fx.c` and `tagpu_posedraw.c`. Equality against a counter that
restarts is not a freshness test: a record published on the frame the old thread died and never
collected would match again that many frames into the new thread's life, and the twin would draw
from pointers into buffers freed and rebuilt in between. Latent exactly one commit out, because
the passes carrying those stamps are the ones 4b-2 ungates. [Found by this landing's review,
2026-09-18; now a file static, as on the GL lane. One backend is chosen per process and can only
degrade to GDI, so the two counters can never both be live.]

**The GL calls are not left to no-op, and that is a decision rather than a tidiness.** With no
context current most of them do nothing. The ones that do not are the ones that read GL state
back: `tagpu_scaffold.c`'s `init_gl` branches on a `GL_COMPILE_STATUS` and a `GL_LINK_STATUS`, so
a lane with no context takes whichever branch the loader's stubs produce, and *both* are wrong —
one logs a shader failure that never happened, the other latches `s_state = 2` and the pass is
dead for the life of the process. A pass that is not called publishes nothing instead, and its
Vulkan twin then refuses out loud (`no hand-over for frame N — the GL pass published nothing`).
The gate went in at the driver in 4b-1's first commit and moves inward one pass at a time.

**Where the line falls inside a pass.** Not at the pass, at the point where the pass stops being
API-independent:

| pass | runs on both lanes | stands down under `renderer=vulkan` |
|---|---|---|
| `tagpu_scaffold_frame` | the packet's anchors walked over the engine's sweep rect into `s_buf`, the per-unit occlusion prediction read back out of it, the four NDC numbers | `init_gl`, the `GL_R8` upload, the blended quad, `tagpu_abshot_*` |
| `tagpu_fps_present` | the lever poll, the 500 ms averaging window, `tagpu_text_frame`'s font latch, the `emit` loop packing eleven cached strings into `s_v` | `init_gl`, the `glBufferSubData`, the triangles, `tagpu_abshot_*` |

Both publish their hand-over **once**, outside the GL block, because the hand-over belongs to
both lanes; in the GL-only code it sat inside the draw.

#### The A/B's arming, and why it had to move first

**The Vulkan capture was armed by the GL capture reaching the disk.** The coupling is deliberate
and documented (`tagpu_abshot.h`): `tagpu_abshot_end` returns 1 only when the PPM was written, and
the caller must not claim the Vulkan half on anything else — because a refused GL write leaves the
PREVIOUS run's `_gl.ppm` lying on the disk, and a Vulkan half diffed against that reports a
capture of a different frame as a port failure. An oracle failure wearing a port failure's
clothes, which is the worst answer an oracle can give.

**On the vulkan-only lane there is no GL half to ask.** `tagpu_abshot_end` is not merely refused
there — it is never called, because the pass that would call it has stood its draw down. So
`wrote` is 0 on every frame and the rule refuses every capture **on the only lane that presents**.
The chain, for the scaffold: `tagpu_scaffold.c:324` polls the trigger → `taking = s_ab &&
!s_abDone` → `s_abFrame = tagpu_abshot_end(...)` → `tagpu_scaffold_overlay(..., &ab)` →
`tagpu_vk_scaffold.c`'s `prepare` → `s_abFrame = ab`. Landing 4's own oracle is a comparison of
two **builds** and has no `_gl.ppm` in it at all, so the guard that makes the two-lane oracle
trustworthy is exactly what prevents the two-build one.

**So the arming is claimed on the intent, and what the rule was buying is established by
construction instead.** Where both lanes run the rule is unchanged and is still what a pass
applies. Where only one does, `ab = taking`, and the target `_vk.ppm` is unlinked — by
`tagpu_vk_ab_arm(tag)`, which the pass calls **in the same statement sequence that latches the
claim**. After that call the file does not exist; it comes back only if `tagpu_vk_shot_finish`
writes it. **Absent means this arming produced no capture** and **present means this arming's**,
on either lane. The eight names live in one table (`s_abFiles`) that both the write path and the
arming read, keyed by the same tag the GL half already passes to `tagpu_abshot_end`, so the name a
capture is written to and the name an arming unlinks cannot drift.

**WHERE the unlink lives is the whole guarantee, and the first version had it in the wrong place.**
It was in `vk_present`, beside the capture it protects, which looked equivalent and was not: the
claim is latched on the gather side whether or not the lane ever collects it, and `vk_present` is
not on the path from that latch. `tagpu_vk_frame` returns before it all through the ~250 ms
bring-up, on a window or GPU-row rebuild, at `ST_FAILED` and at `ST_ZOMBIE`, and `vk_present`
itself returns early on an out-of-date acquire. So the unlink ran on most frames and was missed on
precisely the frames where no capture happens — which is the case it exists for. A guarantee about
timing wearing the words of one about construction, in a landing whose whole subject was that
distinction. [Found by this landing's own review, 2026-09-18; the fix is the placement above,
where one thread reaches both in one basic block and no interleaving separates them.]

**And the unlink's answer is the claim's answer.** `DeleteFileA` can fail — a reader holding the
file open, or a read-only file — so discarding its result left "the file does not exist" asserted
rather than established. `tagpu_vk_ab_arm` returns 1 only when the target is gone (already absent
counts), the pass claims on that as well as on `end`'s, and a failure is named:
`vk: ab: tagpu_scaffold_vk.ppm could not be removed (error 5) - this arming is REFUSED`. The
stale file is then still there, which is the residual — but no claim was granted, no capture was
written, and the log says so.

That is a fact about the filesystem rather than an argument about ordering, which is all the old
rule could have been once the half it read stopped existing. It also closes the refusals that used
to leave a stale `_vk.ppm` behind where both lanes run: a surface whose images do not carry
`TRANSFER_SRC`, two levers armed in one frame, and every path on which the lane never presents.
Until now those were handled by the operator's `rm` in the recipe, not by the code.

#### Measured — one build, one fixture, three runs

`feat-forest` on Two Continents at 1024×768, `ss=1`, one `.ab` at a time. The camera is
`[2950, 1010]` / eye `(2566, 616)` in **all three** runs, and the scaffold's own gather figures are
identical in each: `swept 68x76 tall=151 flat=30 gafFallback=1 junk=0`.

| pass | run | result |
|---|---|---|
| scaffold | `renderer=openglcore` + `tagpu_vk.on` (route D) | **0 px apart**, 190 247 ink px a side |
| scaffold | `renderer=vulkan` (route E) | `tagpu_scaffold_vk.ppm` **written**, and **byte-identical** to the two-lane run's Vulkan half (same md5, 0 px, 190 247 ink px) |
| fps | `renderer=openglcore` + `tagpu_vk.on` (route D) | **0 px apart**, 102 ink px a side |
| fps | `renderer=vulkan` (route E) | `vk: fps: the Vulkan edition is up — 512x256 atlas`, `tagpu_fps_vk.ppm` written; 17 px of 786 432 against the two-lane run's Vulkan half |

**The scaffold's byte-identity is the strong result, and the fps readout's 17 px is not a
weaker one.** The scaffold is a pure function of the eye, the viewport and the map's features, so
two runs at the same camera must agree exactly, and they do. The readout draws **its own
measurement**: the two runs ran at different frame rates, so the digits differ and nothing can
make them agree without measuring the clock instead of the renderer. What makes that a result
rather than an excuse is *where* the 17 pixels are — the ink bounding box is `x 6..50, y 8..14` in
both, the differences are confined to `x 32..43`, and columns 0..31 (the `FPS` label and the
leading digit, 57 ink pixels) agree **exactly**.

**The capture existing is also what proves the gate held.** Had `init_gl` run without a context it
would have latched `s_state = 2`, the pass would have published nothing, and the twin would have
had no hand-over — so there would have been no `_vk.ppm` to diff.

**The unlink is tested on the paths where it is visible, and a succeeding capture is not one of
them** — it overwrites the file anyway, so the positive path says nothing about it. Three
refusals, each with a sentinel planted as `tagpu_scaffold_vk.ppm` first:

| the path | what happened |
|---|---|
| two world passes drawing, one lever armed | the lane refused (`1 A/B levers claimed this frame and 2 passes drew into it - nothing captured`) and the sentinel was **gone** |
| the lane down (`tagpu_vk.off`), so `vk_present` is **never reached** | the sentinel was **gone** — this is the path the first version missed, and it is deterministic rather than a race |
| the target read-only, so the unlink fails | `vk: ab: … could not be removed (error 5) - this arming is REFUSED`, no `shot: wrote` followed, and the claim was not granted |

The middle row is the one worth keeping: with the unlink in `vk_present` the sentinel would still
have been there and `vk-ab.py` would have read it as this arming's capture.

#### Not covered by 4b-1

* **`tagpu_native_frame` and `tagpu_gui_present` still stand down whole**, so under
  `renderer=vulkan` there is still no world and no UI layer — the window is the clear colour with
  the scaffold's overlay or the readout on it when those levers are armed. §2.48's *"100 % one
  colour is the correct result"* therefore still holds for an unarmed instance and no longer holds
  for an armed one.
* **The two remaining entry points are not two more of the same job**, which is what pushed 4b
  into two landings — see [vulkan-only-plan](vulkan-only-plan.html) landing 4b for the seam. In
  short: each world pass publishes its hand-over from **inside** its GL render
  (`terr_publish` is called from `tagpu_terr_render`, not from `tagpu_terr_gather`), so the five
  world passes with a lever of their own — `terr`, `feat`, `fx`, `mark`, `posedraw` — each need the
  scaffold's treatment individually, and the native pass's own composite besides; and the UI
  layer's Vulkan record is emitted
  conditionally on GL twin bookkeeping (`twin_make` / `twin_find` gate `mir_op`), so its record
  cannot be produced at all until the twin table is separated from its GL objects.
* **`ss=1` only**, as everywhere in this plan: `ss=2` has no target on the Vulkan side until 4c.
* **The fps readout needs `mark.on` at launch for the font** on this lane exactly as on the GL one
  — the font reaches the render thread in the frame packet, published at hook 8, which is
  `markown`'s. Under `renderer=vulkan` that hook is installed at DLL attach as always, so the
  requirement is unchanged and not a property of the new backend.

### 2.50 The world draws with no GL context — landing 4b-2 of the Vulkan-only plan

**What this part is.** 4b-1 taught two passes to gather without drawing and fixed the A/B's arming
(§2.49). This part does the other three of the driver's four entry points' worth of work: all five
world passes — terrain, features, effects, markers and units — now run their gather on
`renderer=vulkan`, hand over, and are drawn by their Vulkan twins. `tagpu_overlay.c`'s `gl_draws`
guards one pass now, `tagpu_gui_present`, which is 4b-3.

| pass | two-lane (route D) | vulkan-only (route E) |
|---|---|---|
| terrain | **0 px**, 630 719 ink px | **0 px**, byte-identical (same md5) |
| features | **0 px**, 132 274 ink px | **0 px**, byte-identical |
| effects | **0 px**, 3 327 ink px | draws; cross-run comparison not available |
| markers | **0 px**, 297 ink px | **0 px**, byte-identical |
| units | **0 px**, 2 125 ink px | **0 px**, byte-identical |

One build, `ss=1`, one `.ab` at a time. Terrain and features on `feat-forest` at 1024×768;
markers and units on `selbox-facings`; effects on `fx-lasers`. The effects pass draws transient
projectiles, so two runs agree only if the same ones are alive — its oracle is the same-frame
two-lane A/B, exactly as the frame-rate readout's is, and for the same reason.

#### The shape this landing is made of

**Eleven times, a pass was keyed on GL rather than on what GL stands for**, and each one presented
as a different symptom. They are worth listing together, because the list is the finding:

| where | the test | what it should have asked |
|---|---|---|
| `tagpu_gaf_atlas_create` | `if (a->tex) return 1` | is the atlas LAID OUT (now `a->made`) |
| `tagpu_posedraw.c`'s `unit_ok` | `!m->vao` | does the material name this geometry and agree about the vertex count |
| `tagpu_mark.c`'s `upload_layer` | `glGenTextures(…); if (!s_tex[i]) return 0` | are the layer's BYTES there (the hand-over carries them) |
| `tagpu_mark_render` | `if (textTex)` | is there text to record |
| `tagpu_terr.c`'s `ensure_atlas` | the upload gated the mirror | the mirror IS the buffer the upload was given |
| …and the SAME function's already-built test | `if (s_atlasTex && …)` | is the atlas BUILT (now `s_atlasBuilt`) |
| `tagpu_terr.c`'s gather | `s_maxTex` from `GL_MAX_TEXTURE_SIZE` | the bound of the device that will SAMPLE it |
| `tagpu_render3do.c` | the LUT built only by `_texref` | the LUT is the pass's; the texture is the backend's |
| …and four more of the same two forms | | |

Two forms, then: **a GL handle used as a validity test**, and **a construction reachable only
through one**. Both are invisible while one backend exists, because the handle is always there.

**The counterpart rule that came out of it**: a device limit belongs to the device that will
consume it. `tagpu_vk_max_image_dim()` and `tagpu_vk_max_uniform_range()` read
`maxImageDimension2D` and `maxUniformBufferRange` from the physical device actually bound, and
both return **0 for "no device yet" rather than a default** — the lane takes ~200 ms to come up
while the gathers run from the first frame, so a pass that read 0 as a bound would cache a ruined
atlas for the life of the process. That is measured, not hypothetical: it is what
`terr: atlas built 2176x0 … 0 kept` was.

#### Where each pass's line falls

Not at the pass. At the point where the pass stops being API-independent, and that point is
different in each:

* **terrain** — GL in FIVE places outside its draw, four of which the first reading missed:
  `tagpu_terr_gather`'s own `init_gl`, `ensure_atlas`'s upload, `build_height`'s, and
  `build_hills`' VAO. Each gated around the GL and never around the mirror beside it.
* **features and effects** — the simplest: GL in `init_gl`, the render, and the shader helper. But
  `init_gl`'s TAIL held the atlas's `dim`/`max`/`ents`, so gating it left the atlas unsized;
  extracted as `atlas_setup()`, called from the gather on the other lane.
* **markers** — the record and the draw are two PARALLEL statement streams: `mk_push` and
  `mk_draw` are pure CPU at eight sites with the GL interleaved among them, so this one takes two
  block gates and a gate per GL statement, never around a record append.
* **units** — 150 GL calls in `tagpu_posedraw.c` and **none of them needed a gate**: every
  `tagpu_posedraw_*` entry point is called from the composite the vulkan lane exits before. What
  was missing was the RECORD, not a gate. Three body-path functions (`pd_begin`,
  `tagpu_posedraw_unit`, `tagpu_posedraw_end`) gate their own GL in five contiguous runs, and the
  lane calls them from its own exit.

**And the exit itself moved.** It sat at the top of the composite, which meant the lane never
reached the unit pass's gather — `pdu[]` and the vertex emission, three hundred lines below it.
Audited before moving: there is not one GL call between the four world gathers and the "nothing to
draw" return. The pose view moved with it, into the `s_pv` static that already existed, so both
lanes read one construction rather than two.

#### What is NOT covered by 4b-2

* **`tagpu_gui_present` — the UI layer.** Still gated whole. Its record is built inside the GL
  drain (`twin_make`/`twin_find` gate `mir_op`) and, worse, the record's COLOUR flag is keyed on
  the GL colour twin (`(restored && t->rgb)` reaching `TAGPU_GUICOL_ON`), so a missing handle
  would change the record rather than only the draw. That is 4b-3.
* **`ss=2`** has no target on the Vulkan side; that is 4c, with TA's own surface.
* **The composite** — the world FBO, the cast-shadow map, the shadow and slant redraws, the nano
  wire pass, the key/fill inversion and the resolve — has no counterpart on this lane. Each pass
  draws itself into the swapchain from its own hand-over instead, which is why `tagpu_zoom_publish_view`
  does not run there and the input path stays 1:1.
* **The 300-frame composite stats line** does not run on this lane. The lane has its own instead.
* **Classic++'s restorer never starts on the vulkan-only lane, and nothing says so.**
  `tagpu_terr.c`'s `restore_step` returns on `!s_atlasTex` and `tagpu_gaf.c`'s
  `tagpu_gaf_atlas_restore` on `!a->tex`, both 0 with no GL — and the *published Vulkan* restore
  request is fed from inside those, so the Vulkan lane's restore does not start either. With
  `assets=` on, terrain and every GAF atlas would draw unrestored. **The 0 px figures above were
  all taken with Classic++ OFF**, so they are valid for that configuration and say nothing about
  this one. Two more instances of the same shape, left rather than fixed: the GL restore genuinely
  needs its texture, and giving the Vulkan lane its own restore path is its own piece of work.
* **The engine keeps its own unit rasterise on this lane, and that is deliberate.**
  `tagpu_posedraw_live()` is a PROMISE to the game thread — it acts on a yes by skipping the
  engine's own draw and wiping its composite — and the invariant `tagpu_owndraw.c` states is that a
  stale read may only be stale in the direction of *not* skipping, because "skipping when nothing
  will draw has no write order that produces it". Arming the pass without a GL program produced
  exactly that order: the twin stands down whenever a mirror is missing or the lane is not READY,
  and after the retry budget `render_vk.c` degrades to GDI while `tagpu_vk_owns_present()` stays
  latched — so the game thread would skip and wipe for the life of the process and every covered
  unit would be invisible. `live()` therefore answers **no** on this lane, by construction rather
  than by checking whether the twin happened to draw. The engine does work whose output this lane
  does not present; the redundancy goes when 4c gives the backend TA's surface.
  [Found by this landing's cross-thread review.]

#### The instruments, and what they cost to learn

Four periodic reports now exist, all keyed on the driver's frame so they can be lined up:
`native: vulkan lane handed over frame N: …`, `vk: census: frame N: N drew, M claimed …`,
`posedraw: nothing to hand over for frame N - mirrorWant= win= recording= pubHave=`, and
`vk: unit: frame N: the hand-over carries nunit= …`.

They exist because **one-shot latches lied by omission three times in a row.** A latch reports the
first occurrence and is then silent, so one spent on an ordinary case — the shell, where no posed
unit exists and nothing to publish is normal — hides every frame that matters. Two of them in
sequence sent this landing to the wrong function twice, and a third pair, keyed on two DIFFERENT
counters, produced a flat contradiction (`posed=3` and `win=0`, apparently of one frame). The rule
that came out of it: **prefer a periodic line reporting current state, and never spend a latch on
a case that is ordinary.**

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
