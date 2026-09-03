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
| Weapon fire, explosions, debris | G12e | `fxown`: 2 call-site redirects + 4 leaf detours |
| Smoke, fire, wakes, nanolathe | G12f | `fxown`: one detour on the layer walker |
| Features (trees, rocks, splats, wreckage) | G13a | `featown`: one detour on the feature leaf |
| Terrain tiles + the fog overlay | G13b | `terrown`: two detours; the terrain skip path key-fills the viewport |
| Fog of war *as drawn* | G13c | one shared rule (`tagpu_glsl.h`) in all four native passes |
| Health bars, order markers, group digits, build cursor, band box, selection rect | G13d | `markown`: 6 call-site redirects + 1 detour; bars re-drawn, the rest captured and replayed |
| Mouse cursor position, clicks, minimap view rect, scroll rate | G13e | `tagpu_zoom.c` + the composite |
| The engine's *addressable* viewport at zoom < 1 — clicks, orders and unit picking in the outer ring | G13f | `vpwide`: 3 call-site redirects + 1 more + a 3-site byte patch, all behind `vpwide.on` |
| **Chat, dialogs, side panel, minimap, top bar** | **— never** | screen-space and correct at 1:1 at any zoom; they come through the composite key by design |

**Phases.** Phase 0 (foothold) is complete and Phase B (blit-level GPU units) is verified
complete. Phase D's scene takeover — G13a through G13e — has landed, which is what the table
above records; **G13e is the newest and is still in "awaiting review" on the roadmap.** Two
things remain from the original plan:

- **G11 — the replacement pipeline** (glTF convention + loader + one exemplar unit driven by
  live COB state). This is the project's stated purpose and it is the next real gate. The
  groundwork is real but the two halves **do not meet yet** — worth being precise about,
  because "the slot is proven and the exporter exists" reads as further along than it is:

  | Half | Where it is | The gap |
  |---|---|---|
  | Replacement-mesh slot in the DLL | `tagpu_hires.c` — `gamedir/hires/<defname>.obj` renders instead of the 3DO, hot-reloaded on mtime, engine anchor + body yaw, our shade/palette pipeline. **Proven end-to-end** (a 210-tri dome replaced the solar collector) | speaks an **OBJ subset** (`v` / `f` / `c <palette index>`), **whole-model only** — the source says so: *"piece-wise COB pose = G11"* — and colours are palette indices through the SHD LUT, not textures |
  | glTF exporter | `tools/ta3do` — 3DO + GAF → glTF, standard views, `--undither` | **not on `main`**: it lives on the unmerged `worktree-3do_exporter` branch |

  So G11 is three concrete pieces of work, not a wiring job: **(a)** land the exporter and
  agree one format between it and the loader; **(b)** per-piece pose — the engine hands us
  *fully posed* vertex buffers for its own 3DOs (`PrimitiveStruct+0x22`), which is why the
  native pass needs no rotation maths today, but replacement geometry is not in the engine's
  piece tree, so it must be placed from the posed origin (`+0x16/1A/1E`) and posed turns
  (`+0x10/12/14`) — fields already identified in [Unit → 3DO bridge](unit-3do-bridge.html),
  with the exact order/signs of the rotation composition still to be settled (that is what the
  `tagpu_posedump.on` probe was written for, and it has not been run); **(c)** true-colour and
  translucent materials, which is the part of the stated purpose the palette-index path does
  not reach at all.
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

Every install is **byte-matched first and all-or-nothing**: nothing is written unless every
site in the module still holds the bytes we recorded, so a patched or different exe arms
nothing rather than half of it. Every module installs **once at `DllMain`** and only if its
arm file exists *then* — patching live engine bytes off the frame loop is racy, so there is no
per-frame re-arm. The behaviour flags on top of the patches *are* re-read live.

### 2.1 Draw ownership

| VA | What it is | Module (arm file) | Mechanism |
|---|---|---|---|
| `0x459830` | opaque 3DO rasteriser | `owndraw` (`owndraw.on`) | prologue detour, 5 stolen |
| `0x459C70` | nanoframe/build rasteriser | `owndraw` | prologue detour, 5 stolen |
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
| `0x46A430` | `DrawHealthBars` (`ret 0x10`) | prologue detour, 5 stolen — bars are **re-drawn**, not captured |
| `0x4C1B80` | `KeyboardHotkeySampler(id)` (`ret 4`) | *called by us* — we sample the engine's own SHIFT gate (`0xF9`) rather than reading the key |

### 2.3 Zoom (`tagpu_zoom.c`, `zoom.on`)

| VA | What it is | Mechanism |
|---|---|---|
| `0x41C426` | `call 0x466B70` — minimap view rect, eye clamped at top | call-site redirect; the engine fills the rect, we rescale it by 1/z |
| `0x41C442` | `call 0x466B70` — ...and at bottom | call-site redirect |
| `0x430FAE` | `call 0x4B6A50` — the one site that persists `ScrollSpeed` | call-site redirect; substitutes the player's own value so our scaling can never reach the registry |

**The level comes from two levers, and neither is an engine patch.** `tagpu_zoom.txt` is the
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

### 2.4 Tooling (not part of the render path)

| VA | What it is | Module (arm file) |
|---|---|---|
| `0x45AC20` | `DrawUnit` entry (7 stolen) | `suppress` (`suppress.on`), `tracer` (`tracer.on`) |
| `0x459200` | composite sprite → screen blit | `tracer` |
| `0x469A05` / `0x469BA8` | the two `DrawUnit` return sites | `tracer` |
| `0x4969D2` | the sim tick site (5 stolen) | `scenario` — applies a situation on the game thread |
| `0x485F50` `0x4864B0` `0x422DD0` `0x4224B0` `0x481550` `0x423C50` `0x43F0E0` `0x43AFC0` | `CreateUnit`, `KillUnit`, `FeatureName2ID`, `LoadFeature`, `GetGridPosPLOT`, `SpawnFeatureOnMap`, `ScriptAction_Type2Index`, `NewMainOrder2Unit` | `scenario` — *called by us*, never patched |

### 2.5 State we read, and the two fields we write

| Where | What |
|---|---|
| `0x511DE8` | `TAdynmemStruct**` — the root of everything below |
| `main+0x14357` / `+0x1435B` | unit array begin/end, stride `0x118` |
| `main+0x1435F` / `+0x14367` | HotUnits ids / count (culled to whatever the viewport rect says — the unzoomed one, or the widened one under `vpwide`) |
| `main+0x1431F` / `+0x14323` | eyeX / eyeY |
| `main+0x37E27..0x37E3B` | viewport rect: L, T, R, B, then W, H. **L/T/R/B are WRITTEN while `vpwide` is live** (§2.3b); every pass that means the true 1× rect must call `tagpu_vpwide_true_rect()` rather than read the field |
| `main+0x2C76` | mouse position |
| `main+0x0DCB` | GUI colour byte array (`gui[i]` is an INDEX INTO this, not a palette index) |
| `main+0x37F06` bit0 | `damagebars` registry option |
| `main+0x37F2F` bit2 | `SelBoxes` |
| `main+0x142E7..0x142ED` | minimap rect on screen |
| `main+0x1423B` / `+0x1423F` | view size in map cells (the minimap rect's size comes from here) |
| **`main+0x1434D`** | **`ScrollSpeed` — the ONE engine field the stack writes.** Sim-neutral (a local camera preference no other machine ever sees), driven at base/z, and its save path is guarded (§2.3) |

---

## 3. Known limits — what is still wrong, and what closing it needs

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
