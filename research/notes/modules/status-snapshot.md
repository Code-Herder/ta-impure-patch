# Status, known limits and lessons — snapshot of 2026-10-29

What was ours and what was the engine's, the known limits and the lessons, as the GPU status page last stated them. Current gaps live in the [roadmap](../roadmap.md) and in each module's note.

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

## Native TADR convenience coverage

The [28-row QoL audit](../tadr-port/quality-of-life.md) compares current Impure functions with the
actual DLLs and opt-ins shipped by supported mods. Our build ghosts/`PreviewPieces`, health/
group/order/range markers, world zoom, HUD scaling and replay playback/seeking already cover
part of the interface. Native snapping, queue editing, construction controls and allied panels
remain missing. Escalation/Twilight ship rotation support but no checked content enables it; Escalation's
opponent-facing preview key remains unsupported. Newer development modules are distinguished
from native bundle features. This is a documentation audit, not a new hook or engine write.

## 1. Status — what is ours, what is still the engine's

**THERE ARE TWO RENDERER BACKENDS SINCE 2026-09-18, AND THE PROCESS CREATES NO GL CONTEXT ON
ANY PATH.** The Vulkan backend is the patch and the GDI backend the reference. There is no
`renderer=` key any more — the DLL reads no `ddraw.ini` (§2.8b) — so the Vulkan lane is what
every launch gets, handing the session to GDI late if it will not come up, and the harness lever
`tagpu_gdi.on` forces GDI (`cfg: … renderer GDI (tagpu_gdi.on)`). The `renderer=` spellings
below and throughout the dated records are the ini key those launches used (§2.85). `render_ogl.c` (2 015 lines) and `render_ogl.h` went in the vulkan-only plan's
landing 11-2, ahead of the sixteen passes' GL draw halves rather than behind them -- with the
lane's only other `tagpu_overlay_draw` caller gone, `gl_draws = !tagpu_vk_owns_present()` is
false at every surviving call, so those halves are now unreachable by an ordering and 11-3/11-4
delete dead code. **`tacli shot` moved off the GL present loop with it** and answers on every
lane for the first time -- the flip's trigger family both arms it and, on its NEXT pass, captures,
so the picture is a frame the engine presented in between (capturing in the same pass returns
frame N-1; §2.64). **`tacli glshot` is gone**, and `tascene ab` grabs the game window where it
called it (§2.85).

The paragraph this replaced, kept for its own record: three backends since the Direct3D9
deletion. `renderer=` took `auto`
(the default, and what the generated ini writes), `opengl`/`openglcore`, `vulkan` and `gdi`;
**`direct3d9` and `direct3d9on12` are deleted** (the vulkan-only plan's landing 11-1 —
`render_d3d9.c` and its two headers, **2 387 lines**, plus ~150 more at their call sites).
**`auto` changed lane with them**: it tried Direct3D9 first on native Windows and now goes
straight to OpenGL. Under Wine it never took that branch, so nothing on the reference setup
moved — and nothing on the reference setup can show that it did. The lane
was the fork's, never ours: nothing under `tagpu_` ever called into it and no pass of ours had a
D3D9 half, which is why it is the one member of that plan's deletion set that went whole. An ini
that still asks for it gets GDI and says so once in `tagpu.log`. The GL lane is next to go, in
pieces, and `renderer=gdi` is what the plan makes the stock reference — for RENDERING; see that
plan's item 11 for the two patches that keep it from being stock as a *process*.

**The engine's software frame is UI only.** Inside the world viewport it is a flat fill of one
palette index — the *key* — and the composite is inverted against it: we drop OUR fragment
wherever the engine's frame is **not** the key, so anything it still draws in there shows
through. Measured 99.98 % key with nothing on it but the mouse cursor — which is the engine's
own sprite, drawn under the pointer at every zoom and left alone by the composite (§2.3d).

| What the engine used to draw | Ours since | Owned how |
|---|---|---|
| Units, wrecks, shadows, cloak, waterline | G12a–c, G13n | `owndraw` skips the software rasterisers; `tagpu_native.c` draws them. **The Classic pair — the silhouette and the structure slant — is what the shipped configuration draws again since 2026-09-22, out of `tagpu_vk_unit.c`, and `shadows=` defaults to HARD (§2.83).** G14i's alternative, `tagpu_shadow.c`'s depth map at `shadows=1`, is unported: that module went with the GL backend and nothing produces a hand-over for the Vulkan map. **The silhouette shadow blends once per silhouette PIXEL through a stencil** (G13n) — the engine blits one blackened copy of the composite, so re-using the body's 3-D geometry with depth writes off darkened once per surface the ray crossed: aircraft came out at 0.25 of the ground against the engine's 0.49. The world target's depth attachment carries the stencil plane for it, `D32_SFLOAT_S8_UINT` or `D24_UNORM_S8_UINT` (§2.91) — [shadows & cloak](../shadows-cloak.html) §"What our GL renderer must do" |
| **Units under construction** — the nanoframe scaffold, its fill and its wireframe | G13l | the same pass: ownership no longer stops at `Nanoframe > 0`, the recolour is three per-unit uniforms in the unit shader and the wireframe a line range per unit; a third `owndraw` detour (`0x458DD0`) stops the engine stamping its own copy at the 1× position. A unit under construction casts no shadow, as the engine's does not, and a factory's cargo takes the FACTORY's depth key — the engine z-merges it into the factory's sprite (`0x4B90A0`) rather than sorting it, and on its own tile row it disappeared under the lab; within that key it carries the merge's height offset (§2.87), which is what puts a transport's cargo under the transport. The carry relationship itself — attach/detach `0x48AB70`, and why a *released* unit appears to walk under the plant (stock, measured) — is on [factories](../factory-build.html) |
| Structure shadows (the cached slant projection) | G13k, redrawn 2026-09-22 | `owndraw all` flips the blit's two structure-shadow `je`s behind `g_ssSkip`; the SLANT range of the posed bake is drawn by `tagpu_vk_unit.c`, stencil-masked, before the bodies — see §2.83, §2.1 and [shadows & cloak](../shadows-cloak.html) §"Structure shadows, owned" |
| Weapon fire, explosions, debris | G12e | `fxown`: 2 call-site redirects + 4 leaf detours |
| Smoke, fire, wakes, nanolathe | G12f | `fxown`: one detour on the layer walker |
| Features (trees, rocks, splats, wreckage) | G13a | `featown`: one detour on the feature leaf |
| Terrain tiles + the fog overlay | G13b | `terrown`: two detours; the terrain skip path key-fills the viewport |
| Past the map's edge — the engine's black | G20b | nothing of the engine's changes: the terrain and feature passes draw the map reflected there, in the lab's tone, with the map's own features as its TNT lays them out, read on the loader thread once a load (`edge`, §2.90) |
| Fog of war *as drawn* | G13c | one shared rule (`tagpu_glsl.h`) in all four native passes |
| Health bars, order markers, group digits, ShowRanges labels, build cursor, band box, selection rect | G13d, cursor G13n, order block G13o, text G13p | `markown`: 14 call-site redirects + 1 detour, and **nothing world-anchored is captured any more**. Health bars, the selection rect, the build cursor and drag band box are re-drawn from engine state; **G13o ported the order-marker block** (`tagpu_order.c`) as a game-thread snapshot of the order lists at `0x469BFC`; **G13p ported the text** (`tagpu_text.c`) — the group digit at `0x469CF9` and the `ShowRanges` labels — by calling TA's own glyph blitter `0x4CCF60` with a buffer of ours. Window A is retired and the identity blend LUT with it; the post-fog capture is deleted (the clean cut, §2.39), and `mark.on=nocursor` hands the cursors back to the engine |
| Mouse cursor position, clicks, minimap view rect, scroll rate | G13e, cured G13m, **cursor ours again G17c** | `tagpu_zoom.c`; G13m made the engine's own cursor correct by telling it the truth about the pointer, and the zoom composite stopped touching it (§2.3d). **Since G17c the cursor is DRAWN BY US** — 1× device pixels at every `k`, from the client point the message carried, into the GL UI renderer's sharp layer — and the engine's is erased from the frame by two cooperating exemptions, the UI layer's rect discard and the world composite's `uCurs` ([GL UI renderer](../gui-renderer.html) §17). §2.3d's rule is unchanged and still what keeps the two in the same place |
| Which cursor sprite the engine picks on hover (move / reclaim / …) | G13j | one byte patch in `tagpu_patches.c`; the engine still draws it — see §2.6 |
| The engine's *addressable* viewport at zoom < 1 — clicks, orders and unit picking in the outer ring | G13f | `vpwide`: 3 call-site redirects + a 3-site byte patch behind `vpwide.on`, plus the `0x499221` redirect that also carries the zoom's mouse-point repair and is armed by `zoom.on` too (§2.3d) |
| Terrain in **restored true colour** (Classic++, `tagpu_classicpp.on`) | G14a (spike, 2026-09-04); GPU G14b (2026-09-04); **GLSL G14c (2026-09-05)**; **Vulkan compute (2026-09-27)**; **by neighbourhood (2026-09-27)** | **Today the restorer is Vulkan compute** — `tagpu_vk_restore.c` under `tagpu_restore_core.c`, with a self-test at every launch and a per-driver record when it crashes or fails (§2.102, [The fp32 compute restorer](../compute-restorer.html)) — and **the terrain is restored with its eight neighbours on the tiny model** (§2.103), so a tile's edge is what a restore of the whole map gives there: Two Continents' first screen in 114 ms, the whole map in about 20 s at a 2 ms trickle, with landing 1's per-tile atlas where the neighbourhood atlas does not fit. The rest of this row is how it got there. `tagpu_restoreglsl.c` ran the unditherer's full model as **fragment passes in the game's own GL context** — the shaders of `tagpu_restore_glsl.h`, the weights of `<model>.w32.bin` — sliced from `tagpu_terr.c`'s gather at 12 ms of GPU time per frame under a `GL_TIME_ELAPSED` budget, visible tiles first, painting straight into the terrain pass's RGBA atlas: Two Continents' 5062 tiles in 2.1 s at 59.7 fps, the biggest stock map's 11,561 in 4.2 s, no worker thread, no runtime, no disk. The ONNX Runtime path (`tagpu_restore.c`, G14a/b) was deleted the same day (G14d). **G14e (2026-09-05)**: the cells show as they land, centre-out (the restored atlas's alpha is the flag), and the **feature and effects atlases restore lazily** — `tagpu_gaf.c` queues every atlas miss to the same restorer, each atlas carries an RGBA8 twin the sprite shaders sample where its alpha is 1, keyed texels inpainted by a nearest-ring stand-in in the FILL pass. **G14f (2026-09-05): lit** — the terrain from the engine's height grid (`main+0x14287`, one R8 texture per map, the lab's grid normal per fragment), the units from the posed face normal carried in the vertex stream, the feature sprites from the ground's lambert at their anchor; one rule, `tagpu_glsl.h` `TAGPU_GLSL_LIGHT_FN`, level ground exactly 1.0; the knobs in `tagpu_classicpp.cfg` (`tagpu_classicpp.c`). **G14g (2026-09-05): the unit textures** — `tagpu_render3do.c`'s atlas is a `TAGPU_GAFATLAS` now, every frame in a 4-texel-padded, 4-aligned cell, its RGBA8 twin restored lazily like the sprites' (priority 3) and **mipmapped to level 2**, trilinear and 4× anisotropic, the mips regenerated after each painted batch; the unit shader samples it where its alpha says so. Classic's R8 atlas has the same cells and does not move a pixel (`tascene ab`, and the engine shot byte-identical to G14f's outside the chat). Reads only — see [Classic and Classic++ renderers](../renderers.html) §4c and §5 |
| **Chat, dialogs, side panel, minimap, top bar, the shell** | **Phase E, in progress** ([GL UI renderer](../gui-renderer.html), decided 2026-09-06). **G15a done 2026-09-07**: every UI pixel-writing leaf is observed and the census explains 100 % of what changes on the presented surface across the screen inventory. **G15b done 2026-09-07**: the observed ops are replayed into GL twins of the engine's surfaces and the presented surface's twin is drawn over the world composite — the in-game panel, build pages, bars, option screens, chat and the F4 popup, and the whole shell, at 1:1 Classic; **0 differing pixels and 0 holes under `strict` on every in-game stop of the inventory at 1024×768 and at 1920×1080** (§2.3e). **G15c done 2026-09-07**: the rest of the in-game frame reached and measured — ARM and CORE, both resolutions, the HUD strings, the popups and the option screens over the world at 0.5× and 2×, the minimap under motion: 32 of 32 stops at 0/0/0 in all four runs, no DLL change. **G15d done 2026-09-07**: the shell across the 640×480 context switch, three game→shell→game cycles in one process, clean — the publisher drops batches while the render thread is dead or crawling (the switch stops it and it crawls out of a game), skips the stale queue after the new GL context, retires the main offscreen's dead entry, and resolves the twin through the palette the frame is *presented* with, not `main+0x143A7` (the engine gamma-scales the presented one) | the engine's surface is still drawn and still the fallback beneath the twin (nothing is suppressed in phase 1); the cursor stays the engine's; **G15e done 2026-09-08 and its Q2 diff closed 2026-09-09** (§2.3e and [GL UI renderer](../gui-renderer.html) §14: the UI restores inside the Q2 bar at both resolutions, in the shell and in game, and the exclude list does not grow); the world passes reading `main+0x143A7` were wrong at Gamma ≠ 12 — **fixed 2026-09-09**, §2.3f, by resolving the world through the presented palette, and **replaced by G20c (2026-09-23)**: the world takes the unscaled table and the factor is applied once, at the world composite (`tagpu_vk_world.c`). How often Gamma ≠ 12 is **not** a property of the setup: the template and all 58 instance prefixes are one inode that wine rewrites at launch, now reading 12, i.e. `paldiff=0` and no seam ([GL UI renderer](../gui-renderer.html) §15). **G17a's owner decision — that the world stays on `main+0x143A7`, the lab being the reference — was taken in parallel with that fix and is SUPERSEDED by the owner on this landing** ([GL UI renderer](../gui-renderer.html) §15 "Not closed here"): at `Gamma` 12 the factor is 1.0 and the two tables are bit-identical, so the cost it was rejected on is zero today; the lab staying on `palette.pal` is the residual, stated there; on by default since 2026-09-08 (§2.8; `tagpu_gui.off` turns it off, `tagpu_gui.on` still carries the tokens); **phase 2 — the UI scaled — was designed 2026-09-08** ([GL UI renderer](../gui-renderer.html) §13, roadmap G17a–e: the 1× mirror kept as the oracle with a device-res sharp layer beside it, a string op for text, our cursor at 1× device size, the minimap regenerated with the engine's dots) |

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
  | Replacement-mesh slot in the DLL — **DISABLED AND DELETED 2026-09-19 (landing 11 D3), on the owner's ruling: out of scope, implementation TODO. It had already been producing nothing for several landings; see §2.80. What follows is the design record.** | `tagpu_hires.c` (glTF 2.0 loader) + `tagpu_hires_draw.c` (its own GL program) — `gamedir/hires/<defname>.glb` or `.gltf` renders instead of the 3DO, hot-reloaded on mtime. One static VBO per model, triangles sorted by material, one draw per material; per-pixel lighting against the engine's own light direction, normal maps through a derived TBN, metallic/roughness, mipmapped true colour. **Posed per piece from the engine's live COB state**, bound by glTF node name, so it walks, aims, recoils and honours `HIDE`. Shares the native pass's FBO, depth keys, scaffold, fog, waterline and silhouette shadow. **Was proven end-to-end** (a 1577-tri 6-material Peewee, 20 of them in a fight vs engine-drawn AKs; the pose reconstructs the engine's own posed vertex buffer to 2e-5 model units) — [model import](../model-import.html) | No skins, morph targets, glTF animation, sparse accessors or texture wrap modes (UVs clamp); PNG images only; 48 posable pieces. Team colour arrives as a `baseColorFactor` authored into the model by hand — the exemplar's `pw_team` and `pw_team_chest` materials carry ARM blue as a linear factor — and nothing drives it from the owning player. The order two simultaneous piece-turn axes compose in is a documented guess, and COB `MOVE` read zero in every sample |
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
  the omission Gate 0 found ([GPU posing](../gpu-posing.html) §0). The replacement pass still makes
  it, so a glTF unit is posed without the terrain's tilt.]* It also caught the
  replacement pass applying the body yaw *inverted*, which had been invisible because the test
  scenario was parked at facing 90, one of the four facings where the two agree. Derivation and
  authoring guide: [model import](../model-import.html); **(c)** true-colour and translucent
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
  control and what it does not establish: [field notes](../field-notes.html), "Environment &
  toolchain"; the probe is `tools/vkprobe.c`.
- **G9 — the MP-safety replay byte-diff.** Mostly formalisation now: 200v200 measures 59.7 fps
  and every hook is read-only over the sim, but this is the gate that *proves* the native stack
  is sim-neutral, and the byte-diff needs an unlocked session. It has been deferred several
  times.

**Numbers that are load-bearing.** 200v200 at 59.7 fps. Terrain 0-px parity against the
engine's own blit. Fog overlay 99.06–99.39 % lit-vs-grey agreement. Viewport black fraction
0.0019 at 1×, 0.0003 at 0.5×, 0.0002 at 0.35×. Engine surface 99.98 % key (112 px of 630 784).

## 3. Known limits — what is still wrong, and what closing it needs

### 3.0 Closed since the last pass: the interior cracks at zoom-out

**Reproduced, root-caused and fixed** ([terrain & depth](../terrain-depth.html) §7.6, the *fifth*
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

- **A unit's team colour is its owner's logo colour, read by the publisher.** The engine takes it
  from the owner's player record, `PlayerInfo+0x96` (`exe-reverse-engineering.md`, the unit face rule at `0x459B0F`);
  `PK_UNIT.logo` carries that byte (bounded: owner < 10, the PlayerInfo pointer range-checked),
  the unit pass keys the bake's material on it, and the build ghost keys on `local_logo`. The
  colour is the player's choice and not the slot: a skirmish that gives slot 0 colour 4 and slot 1
  colour 0 draws the same colours as the engine [MEASURED 2026-09-29, retail, four players on
  colours 4/0/2/7, ours against the engine's frame]. The table read through `face_texframe` is the
  sequence's `{GAFFrame*, u32}` pairs; before 2026-09-29 it was stepped at 0x18 from frame 0,
  so every colour but the first fell back to a flat blue face (TA Zero, every unit blue).
  A two-peer network game agrees too [MEASURED 2026-09-29, retail, Two Continents, host on colour
  3 and joiner on colour 1]: each peer reads colours 3 and 1 from its own `PlayerInfo` records,
  and its own structures, the other player's commander and the other player's replicated
  structures all draw in the engine's colours. The earlier observation that every peer drew its
  units in player 0's colour was of the slot-indexed lookup this replaces. Wreck (feature) models keep frame 0. When the logo does not validate (an owner
  or PlayerInfo that is not one of the ten, or a byte past the ten colours) the unit falls back to
  its owner id. **A divergence from the engine:** a colour at or past a sequence's frame count
  makes the engine paint nothing (`0x4B7F30` returns 0), and `face_texframe` clamps to the last
  frame instead [unmeasured whether any stock sequence has fewer than ten frames for a model
  that is drawn; the ARMCOM torso's `glow` has 8].

- **Nothing in this repository has ever been measured on a Windows GL driver, and the first run
  on one found two bugs — 2026-09-10.** `renderer=openglcore` fell back to GDI on a real ICD
  because `glGetIntegerv` was fetched through `wglGetProcAddress`, which returns NULL for the
  OpenGL 1.1 entry points on Windows but not under Wine; and the GL UI layer painted its stale
  twin over the entire intro movie, because no drawing routine touches a movie frame (the
  decoder writes the offscreen and the ordinary flip presents it) and so the publisher never
  saw one. Both were fixed on GL ([GL UI renderer](../gui-renderer.html) §21), and the Vulkan
  lane's movie is §25 there. The point that survives is the **gap in the harness**: `tacli`
  runs every instance in a wine prefix and skipped the intro movies, so neither code path had
  an oracle at all. `tacli create <i> --intro` plays them now. The Windows VM
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
  accumulate. See [features](../features.html) §5 and [the GAF sprite atlas](../atlas-packing.html) for
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
  construction ([engine map](../exe-reverse-engineering.html) §`0x42D5xx`). The walk itself gained
  **no** per-node `IsBadReadPtr`: a readability probe is a fact about the past and would have
  made the fault rarer rather than impossible (`CLAUDE.md`, *Fixes must be safe by construction*;
  [thread-safe destruction](../thread-safe-destruction.html) §2 on what a Mode B read owes). The
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
  [shadows & cloak](../shadows-cloak.html) §"Structure shadows, parity". The lab's Classic lane
  draws the slant for structures since the same day ([renderers](../renderers.html) §1).
- **A replacement mesh casts a Classic++ shadow and does not receive one** (G14i,
  [renderers](../renderers.html) §2.4): `tagpu_hires_draw.c` lights with its own GGX rule and has no
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
| **Palette-space blend tables not reproduced.** Feature shadows are a true 50 % RGB blend where the engine remaps through `ALP[src·256 + dst]`, which quantises to palette entries and therefore *dithers* — the diff map shows a speckled crescent on each tree's shadow side, clean bodies. The effects pass's LHT flash is the same class of deviation | [Features](../features.html) §8, [Effects](../effects.html) | sample the real ALP/LHT tables in the shader instead of blending in RGB |
| The engine used to shade-remap its **own** overlays under the fog's grey band; we no longer do | [Terrain & depth](../terrain-depth.html) §7.7 | visible only if a health bar were drawn on out-of-LOS ground, which the engine does not do — probably nothing to do |
| The options screen shows the **scaled** `ScrollSpeed` while zoomed | §2.3 | cosmetic; it is the rate scrolling is actually running at |
| Body 2 of `0x46A610` (animated GAF wreckage) is written but was never reached live | [Features](../features.html) §8 | a fixture that produces animated wreckage |
| The engine's own feature `+0xFF` bit3 LOS gate never fired on the maps tested (`los-skip` is always 0) | [Features](../features.html) | a map that exercises it, or accept it as dead on stock content |
| Layer 8 and the `0x4FD588` particle class were never observed live (the latter is emitted by teleport and `0x472630`) | [Effects](../effects.html) | a fixture that emits them |
| The sim-side particle update walker's entry is not pinned | [Effects](../effects.html) | nothing — only the DRAW is owned, by design |
| Cloak polish (true alpha) | [Shadows & cloak](../shadows-cloak.html) | a cloakable unit in a fixture |
| ~~**The hires stencil shadow is correct by construction but unproven on content.**~~ **MOOT since 11 D3 deleted the pass** (owner's ruling: glTF out of scope). `tagpu_hires_draw` got the same per-unit mark/blend as the 3DO path (G13n), because it blended `alpha 0.5` per fragment with depth writes off and a replacement mesh is denser than a 3DO. The only replacement mesh that exists is a Peewee, which is single-layer from above — the A/B (npass forced to 1 vs 2, `hires-peewee`, 10 frames each) shows the peak staying at 0.44–0.52 and no 0.25 mode either way, exactly as the stock Peewees were bit-identical across the 3DO fix. So the change is verified not to regress, and its benefit is unverified for want of a two-layer glb | `tagpu_hires_draw.c` | a replacement mesh with overlapping groups from above |
| **A structure's slant shadow is not punched out by its own body at +5 px.** The engine erases the shadow where the body sprite sits 5 px right of itself (`0x4B9D70`), then draws the body at +0; we draw the shadow and cover it with the body at +0, so a strip up to 5 px wide along each building's right edge is shadowed where the engine shows ground | [Shadows & cloak](../shadows-cloak.html) §"Structure shadows, owned" | a stencil pass — **the FBO carries a stencil since G13n**, so this is now cheap where it was not — or accept it |
| ~~A **replacement-mesh structure** casts every piece~~ — **MOOT since 11 D3 deleted the pass**; recorded because a revival inherits it. The engine's raster takes only pieces with prim flag bit1 | `tagpu_hires_draw.c` `uSlant`, deleted | zeroing the shadow pose of the pieces whose engine prim lacks bit1 — no replacement structure ever existed to test it on |
| A **nanoframe** casts nothing until complete; the engine drew the cached shadow of its finished pieces (teal under `terrown`) | `tagpu_native.c` (nano > 0 is not listed) | listing nanoframes shadow-only, pieces with bit1 |
| **The structure-shadow flip is installed at attach and never undone**, like every code patch. Remove `tagpu_native.on` live under `owndraw all` and buildings lose their shadows along with their bodies (that state already draws no unit at all — *ta-drive*, "a stale owndraw.on is worse than none"); re-creating the file brings both back within 30 frames. With `writeback` armed beside `owndraw all` the engine's COMPLETED branch blits a real silhouette for a structure — teal under `terrown`, a silhouette instead of the slant without it | `tagpu_owndraw.c` | nothing for play; an A/B that needs the engine's cached shadow launches without `owndraw all` |
| ~~**Classic++ restored art is not exactly linear in the Gamma factor.**~~ **CLOSED by G20c (2026-09-23).** The restorer expanded the indexed atlas through the presented palette, so at a factor other than 1.0 the model saw brighter art than it was trained on (MEASURED 2026-09-09 at factor 1.5: more than 6 levels on 0.68 % of the viewport, max 19). It now restores from the base atlas, built from the unscaled table, and the factor is applied once to the finished world through the engine's own curve: a live 1.0 → 1.5 change is exact on 95.8 % of the world and within ±1 on the rest | `tagpu_pal.c`, `tagpu_vk_world.c`, §2.3f | — |
| ~~**A Gamma change mid-game costs a full Classic++ re-restore**~~ **CLOSED by G20c (2026-09-23)**: nothing in the world is restored or rebuilt when the factor moves — the curve is one 256-byte upload per slot, on the frame that carries the change (§2.3f). The UI's twin still re-restores on a presented-palette change, which in play is the Gamma alone | `tagpu_vk_world.c`, §2.3f | — |
| ~~**`tagpu_fog_at`'s dimension bound is 512 and the producer's cap is 1024**~~ **CLOSED 2026-09-10.** The gate now bounds against `tagpu_fogwide_dimcap()`, published by the code that does the allocating and a high-water mark so it can only ever be too generous; the engine-descriptor bound in `tagpu_native.c` became the shared `FOGW_ENGINE_DIMCAP`. The three numbers that had to agree are one. The grid itself is no longer a fixed square either — it is sized from the window the screen asks for at the worst eye residue (`fogw_capacity`), **212 KB for the set at 1920×1080 and 84 KB at 1024×768 against the old 6144 KB in every session**, and the set is grown behind `tagpu_reclaim`'s quiescence fence because the render thread holds `s_hold` across a whole frame with no lock | `tagpu_fx.c` `tagpu_fog_at`; `tagpu_fogwide.c` `fogw_capacity`/`fogw_alloc`/`fogw_retire` | the grow path has never run from a real video-mode change — only from a temporary probe. A session that goes game → shell → game at a different resolution is the untested path |
| **The unit array's `beg`/`end` pair is read across the thread boundary as two unsynchronised loads** and then used as the bounds of a `+= 0x118` walk that dereferences every slot. **Audited 2026-09-11** ([cross-thread engine reads](../cross-thread-engine-reads.md) §5): of the nine files the sweep counted, six read it on the render thread below the teardown gate (`tagpu_native.c`, `tagpu_scaffold.c`, `tagpu_mark.c`, and `tagpu_overlay.c`'s `log_units`/`probe_unit_model`/`writeback_paint` — all six removed by the frame packet's landing 3, and `log_units` has since left `tagpu_overlay.c` entirely: the vulkan-only plan's landing 10c-3 made it `roster_log` in `tagpu_packet_pub.c`, on the game thread), three read it on the game thread (`tagpu_order.c`, `tagpu_scenario.c`'s resolvers, `tagpu_weapons.c`), and one reads it on the render thread *before* the gate but only on a `tacli` trigger (`tagpu_tracer.c`; `tagpu_cat.c` was the other until landing 10c-1 moved it to the game thread). The fence covers the teardown — `0x485980` frees the array and nulls `begin` inside the cascade — but **not the next level's load**: `0x4854A0` stores `begin` (`0x485525`), memsets the whole array, makes two more allocations, and only then stores `end` (`0x4855D6`), which is the only store to `main+0x1435B` in the binary, so `end` is never nulled. For the length of that memset the pair is (new `begin`, last game's `end`): the same block and nothing shows, a lower block and the walk runs past the new allocation into freed memory. Alignment plays no part in it; a torn load is a rarer variant of the same outcome | the six render-thread `OFF_BEGIN` reads, `tagpu_native.c`'s first | **`(end - beg) % 0x118 == 0` is a filter, not a fix** — random skew passes it 1 time in 280 — and is NOT the check `tagpu_order.c:753` / `tagpu_native.c:3938` already make (`(u - beg) % UNIT_STRIDE` on a *candidate pointer*). The **exact** relation `end == beg + (count-1)*0x118`, with the slot count the engine publishes at `main+0x14351` *before* `begin`, refuses every skew that is not benign and is worth adding as a refusal layer; against a torn load it is still only a filter. The by-construction answer is the fence `tagpu_reclaim` already applies to `0x491B60`, applied to `0x4854A0`: hold the reader off for the length of the allocation, or publish the pair from its post hook into aligned fork statics. A change here is a `high` review |
| **`tagpu_overlay_draw`'s two early returns above the teardown gate still run `writeback_paint`**, which dereferences `*(char**)(u + U_OBJ3DO)` — after `tagpu_reclaim_pass_begin` has already published `s_completed = s_started` for that pass. The gate's own comment says the writeback must not run during a teardown, and those two paths sit above it. Found by the G13t re-review while checking the latch that closed the same hole one line lower | `tagpu_overlay.c` lines 583 and 585, `writeback_paint` | reachable only with `tagpu_writeback.on` **and** (`tagpu_overlay.off` or `s_state != 1`), so it is debug-lever-only and a no-op in play — which is why G13t recorded it instead of widening its own diff. The fix is the same shape: those returns must consult the latched flag too |
| ~~**`tagpu_reclaim.c` claims every reader of the UnitDef array is on the game thread**, and uses that to justify leaving the free at `0x42DCCB` unhooked. But `tagpu_cat_frame`, `tagpu_weapons_frame` and `tagpu_scenario_frame` all run from `tagpu_overlay_draw`, on the RENDER thread, above the gate.~~ **CLOSED 2026-09-18 by landing 10c-1**, and closed the way the row asked for: the justification was not corrected, the premise was made true. All three now run on the GAME thread, from the engine's own flip, so `tagpu_reclaim.c`'s claim holds as written and the free at `0x42DCCB` needs no hook. The move was made to reach `renderer=gdi`; closing this was the side effect | `tagpu_reclaim.c` ~line 215, and `tagpu_triggers_frame` in `tagpu_overlay.c`, called from `tagpu_gui_hook.c`'s `before_flip` | nothing left to do |
| **Past about 7680×4320 the wide fog grid is clamped and the outer ring smears again.** `tagpu_fogwide`'s three buffers are `FOGW_MAXDIM` square and allocated ONCE — it publishes a pointer into `s_pub` to the render thread while the game thread builds into `s_build`, so a buffer grown under a zoom change would be a use-after-free — and 1024 cells covers the window a real screen asks for (485 at 3840×2160, 645 at 5120×2880, 965 at 7680×4320, all MEASURED against the arithmetic 2026-09-09). Past that the clamp takes its trim off both ends, so the view's centre keeps its cover and only the edge returns to the border-cell smear | `tagpu_fogwide.c` `FOGW_MAXDIM` | a bigger allocation, or a publish handshake that makes growing one safe; neither is worth it for a screen nobody has |
| The **unit** pass's `MAXNV` is the first fixed budget a very wide zoomed-out view meets, now that the terrain's and the feature pass's are the screen; the unit gather itself grows to the packet (§2.86) | `tagpu_native.c` | measure how many vertices a 4K 0.25× view over a full map actually emits, then size or bail deliberately. The feature pass's `MAXBV_BODY`/`MAXBV_SHAD` were this row's other half until 2026-09-10; they are gone — `tagpu_feat.c`'s buckets `realloc`-double from `BV_BODY_0`/`BV_SHAD_0` behind `feat_room()` up to a 16 MB ceiling, and a 4K 0.25× view on Town & Country grew them to 65536/32768 verts with `DROPPED(full=0)` |

### 3.3 Open questions, not limits

- **The MAPPED anomaly** ([terrain & depth](../terrain-depth.html) §5.2) — two LOS stores that will
  not reconcile. Unresolved, and moot for rendering.
- **A scenario `move` order across the Two Continents forest** walks the unit to the map's west
  edge instead of the target. Undiagnosed; a direct right-click order works, so no fixture
  depends on it.

## 4. What the work taught us

These are the transferable parts — the reasons things are shaped the way they are.

**Own the draw four ways, and pick the cheapest that is honest.** (These are the four *ways*.
[Own the draw](../own-the-draw.html) numbers its rules differently — its first is the **arming
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

## Where to go next

- [GPU renderer roadmap](../roadmap.html) — the gates, the chronological log, the decisions locked.
- [Own the draw](../own-the-draw.html) — the arming rule and the three take-over rules, in full,
  with the byte-level detail and the stub shapes.
- [Frame composition](../frame-composition.html) — `DrawGameScreen 0x468CF0` and the per-unit path.
- [Terrain, features & depth](../terrain-depth.html) — the OFFSCREEN, the fog, what is left in the viewport.
- [UI markers](../ui-markers.html) — every engine-drawn per-unit marker and how G13d took them.
- [Field notes & gotchas](../field-notes.html) — the things that cost time.
