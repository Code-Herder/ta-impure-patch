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
through. Measured 99.98 % key with nothing on it but the mouse cursor, and the cursor is
moved by the composite too.

| What the engine used to draw | Ours since | Owned how |
|---|---|---|
| Units, wrecks, shadows, cloak, waterline | G12a–c | `owndraw` skips the software rasterisers; `tagpu_native.c` draws them |
| **Units under construction** — the nanoframe scaffold, its fill and its wireframe | G13l | the same pass: ownership no longer stops at `Nanoframe > 0`, the recolour is three per-unit uniforms in the unit shader and the wireframe a line range per unit; a third `owndraw` detour (`0x458DD0`) stops the engine stamping its own copy at the 1× position. A unit under construction casts no shadow, as the engine's does not, and a factory's cargo takes the FACTORY's depth key — the engine z-merges it into the factory's sprite (`0x4B90A0`) rather than sorting it, and on its own tile row it disappeared under the lab |
| Structure shadows (the cached slant projection) | G13k | `owndraw all` flips the blit's two structure-shadow `je`s; the native pass emits the slant projection from the posed prims — see §2.1 and [shadows & cloak](shadows-cloak.html) §"Structure shadows, owned" |
| Weapon fire, explosions, debris | G12e | `fxown`: 2 call-site redirects + 4 leaf detours |
| Smoke, fire, wakes, nanolathe | G12f | `fxown`: one detour on the layer walker |
| Features (trees, rocks, splats, wreckage) | G13a | `featown`: one detour on the feature leaf |
| Terrain tiles + the fog overlay | G13b | `terrown`: two detours; the terrain skip path key-fills the viewport |
| Fog of war *as drawn* | G13c | one shared rule (`tagpu_glsl.h`) in all four native passes |
| Health bars, order markers, group digits, build cursor, band box, selection rect | G13d | `markown`: 8 call-site redirects + 1 detour; bars re-drawn, the rest captured and replayed. The waypoint star's two sites are wrapped with an identity blend LUT so it composites opaque instead of against the fill key (§2.2) |
| Mouse cursor position, clicks, minimap view rect, scroll rate | G13e | `tagpu_zoom.c` + the composite |
| Which cursor sprite the engine picks on hover (move / reclaim / …) | G13j | one byte patch in `tagpu_patches.c`; the engine still draws it — see §2.6 |
| The engine's *addressable* viewport at zoom < 1 — clicks, orders and unit picking in the outer ring | G13f | `vpwide`: 3 call-site redirects + 1 more + a 3-site byte patch, all behind `vpwide.on` |
| **Chat, dialogs, side panel, minimap, top bar** | **— never** | screen-space and correct at 1:1 at any zoom; they come through the composite key by design |

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
| `0x469BD7` | `call 0x471F90(ctx,8)` — hook 8, opens capture window A | call-site redirect |
| `0x469D2C` | `call 0x471F90(ctx,9)` — hook 9, closes window A | call-site redirect |
| `0x469EC5` | `call 0x4BF8C0` — build-cursor rect (window B) | call-site redirect |
| `0x469F1E` | `call 0x4BF8C0` — drag band box (window B) | call-site redirect |
| `0x439516` | `call 0x439740` — target sprite, from the route-dot drawer | call-site redirect; brackets the call with an identity blend LUT |
| `0x439C7D` | `call 0x439740` — target sprite, from the walker's bit-3 dispatch | call-site redirect, same wrapper |
| `0x46A430` | `DrawHealthBars` (`ret 0x10`) | prologue detour, 5 stolen — bars are **re-drawn**, not captured |
| `0x4C1B80` | `KeyboardHotkeySampler(id)` (`ret 4`) | *called by us* — we sample the engine's own SHIFT gate (`0xF9`) rather than reading the key |

**Why the star needs a wrapper at all.** `0x439740` is the only alpha-composited marker: it
reads the destination pixel through `tab[(src<<8)|dst]`, and inside our viewport the
destination is the fill key, so it blended the waypoint star with palette 254's bright cyan
and rendered it teal. Wrapping the two call sites with an identity LUT turns that one
composite into a copy. The swap is scoped to the **call**, never the frame, because
`[globals+0xC0]` owns a heap buffer the engine allocates, frees and refills — full
derivation in `exe-reverse-engineering.md` §"The blend LUT and the marker composites".

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

**Opt-in and off by default.** Nothing here writes a byte unless `tagpu_vpwide.on` existed at
DLL attach, the true rect verified, *and* a zoomed-out view is live.

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

**The cursor is deliberately not a reader of this rect, and that is the design point.** The
engine draws its sprite wherever `GetCursorPos` reports and the composite moves it back under
the pointer from there, which only works while that position is inside the 1× viewport, over the
terrain key fill. **The engine can NAME more than it can DRAW ON**: in the ring a widened `u`
lands on the side panel — where the sprite is composited over panel pixels the composite must not
stamp into the world — or off the surface entirely. So `tagpu_zoom_to_engine_draw()` keeps the
ring identity for that one poll while the messages carry the widened `u`. The same ambiguity is
why the `0x498DA0` stub takes `g_ddraw.cursor` rather than trusting the engine coordinate: at
0.5× the range `[0,128)` is reached both by a ring pointer and by a pointer on the panel, and
without the true pointer to settle it a click in the world lands in the minimap's click rect.

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

**And the right-edge mouse scroll cannot fire at zoom > 1 — only the right one.** Found while
documenting this, not by the change: TA's scroll poll (`0x41CF10`, mapped in
[exe RE](exe-reverse-engineering.html)) fires on *hotkey* or *pointer on an exact screen edge*,
and the mouse half is an **equality on the outermost pixel** — `x == 0`, `y == 0`,
`x == screenW − 1`, `y == screenH − 1` — read through `GetCursorPos`, which
`fake_GetCursorPos` answers with the **unzoomed** `u`. Three of those four screen edges lie
*outside* the viewport rect (`L=128`, `T=32`, `B=screenH−33`), so the transform passes them
through as identity and they still scroll. The screen's right column, though, *is* the
viewport's right column, so it is contracted toward the centre: measured at 2× on 1024×768 a
pointer at `x=1023` reaches the engine as **800**, and `x == 1023` becomes unsatisfiable. So
zoomed in, scrolling right needs the keyboard (`0xF6`) or the minimap. The narrow fix is to
keep `tagpu_zoom_to_engine_draw()` at identity on the outermost screen column and row, which
would also put the drawn cursor there — G13e's cursor path, so it wants its own verification
rather than a quiet ride-along on this change.

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

### 2.4 Tooling (not part of the render path)

| VA | What it is | Module (arm file) |
|---|---|---|
| `0x45AC20` | `DrawUnit` entry (7 stolen) | `suppress` (`suppress.on`), `tracer` (`tracer.on`) |
| `0x459200` | composite sprite → screen blit | `tracer` |
| `0x469A05` / `0x469BA8` | the two `DrawUnit` return sites | `tracer` |
| `0x4969D2` | the sim tick site (5 stolen) | `scenario` — applies a situation on the game thread |
| `0x485F50` `0x4864B0` `0x422DD0` `0x4224B0` `0x481550` `0x423C50` `0x43F0E0` `0x43AFC0` | `CreateUnit`, `KillUnit`, `FeatureName2ID`, `LoadFeature`, `GetGridPosPLOT`, `SpawnFeatureOnMap`, `ScriptAction_Type2Index`, `NewMainOrder2Unit` | `scenario` — *called by us*, never patched |

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
| `main+0x2C76` / `+0x2C7A` | mouse position, two dwords (`+0x2C78` is the high half of x, not the y) |
| `main+0x0DCB` | GUI colour byte array (`gui[i]` is an INDEX INTO this, not a palette index) |
| `main+0x37F06` bit0 | `damagebars` registry option |
| `main+0x37F2F` bit2 | `SelBoxes` |
| `main+0x142E7..0x142ED` | minimap rect on screen |
| `main+0x1423B` / `+0x1423F` | view size in map cells (the minimap rect's size comes from here) |
| **`main+0x1434D`** | **`ScrollSpeed`** — sim-neutral (a local camera preference no other machine ever sees), driven at base/z, and its save path is guarded (§2.3) |
| **`*(0x51FBD0) + 0xC0`** | **the blend LUT pointer. WRITTEN, transiently, and this is the one field we write that is NOT in `main`.** Swapped to an identity table across the target sprite's draw and restored on return, so the star composites as a copy (§2.2). Game thread only, bracketed around one call that always returns, restored only if ours is still installed, with a belt-and-braces restore at hook 8. It must never be left installed across a frame: `0x4BA5C0` allocates that buffer, `0x4BA5F0` frees it and `0x4BAAD0` refills 64 KB through the pointer, so a stale one of ours would be clobbered or cross-heap-freed |

### 2.6 Engine byte patches — no hook, no state (`tagpu_patches.c`)

Not detours and not redirects: bytes rewritten once in `DllMain` through `VirtualProtect`, each
written only if the site still holds the value we recorded. They own no state and run no code of
ours, so they are listed here rather than in §2.1–2.5. The table of them with the before/after
bytes is `field-notes.md` §"Our engine patches".

| VA | What it is | Mechanism |
|---|---|---|
| `0x4266A7` | the `jne` that reaches TA's startup DirectX-version warning | `75` → `EB`, so the warning is always skipped |
| `0x43E50C` | `je 0x43EB02` — the `Interface Type == 1` arm of `0x43E490`'s order-1 (contextual) case, which suppresses `cursormove`, `cursorreclamate` and the rest | `0F 84 F0 05 00 00` → `90` ×6, so the contextual cursor always takes the classic branch. **Cursor only**: `0x43E490` has exactly one caller (`CorretCursor_InGame 0x48D220`) and no address literal in the image, while left-vs-right ordering reads `main+0x37EFA` at four other sites. `tagpu_curs.off` opts out, read once at attach |

Neither writes engine state, so neither appears in §2.5. The engine still draws the cursor
itself — the composite only moves it (§1); what the patch changes is which sequence out of
`cursor_ary` (`main+0x1487F + idx*4`) the engine hands to `SetUICursor 0x4AB400`.

---

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
**opt-in**: `tacli arm <i> vpwide.on`, at launch like every other code-patching pass.

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
| **A structure's slant shadow is not punched out by its own body at +5 px.** The engine erases the shadow where the body sprite sits 5 px right of itself (`0x4B9D70`), then draws the body at +0; we draw the shadow and cover it with the body at +0, so a strip up to 5 px wide along each building's right edge is shadowed where the engine shows ground | [Shadows & cloak](shadows-cloak.html) §"Structure shadows, owned" | a stencil pass (the FBO's depth attachment has no stencil today), or accept it |
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
4. **Move it in a pass you already own** — *the fourth rule.* A pass that already arbitrates
   between your pixels and theirs can move theirs. The mouse cursor failed every capture test
   (it is blitted with a **NULL draw context**, which makes the callee build its own offscreen
   over the primary surface), but the composite already decides per pixel whether the viewport
   shows ours or the engine's, so it simply reads the cursor's texels from where the engine put
   them and paints them where the pointer is.

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
99.98 % key with nothing on it but the cursor" is what makes the composite able to move the
cursor at all. It was measured (112 px of 630 784), and it is the thing to re-measure if
anything engine-drawn ever appears in the viewport again.

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
