# Frame-rate readout and HUD scale

The FPS readout and HUD scaling.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.14 The frame-rate readout (`tagpu_fps.c`, OFF by default, `tagpu_fps.on`) — 2026-09-09

A row on the render-options screen (Off|On) and the module that draws it. **No engine address, no
patch, no engine read at all** — it counts our own presents and draws over the finished frame.

| site | what we do there | thread |
|---|---|---|
| `tagpu_overlay_draw`, after the UI flush | `tagpu_fps_present()` — the readout, drawn **above** the UI layer so the side panel and dialogs could not hide it. **Since the clean cut there is no UI layer and no `tagpu_gui_present`** (§2.81), so this is the last thing drawn over the world and the only 2D overlay left in the frame; its position in the order is now arbitrary rather than load-bearing | render |
| `tagpu_overlay_draw`, the context-change branch | `tagpu_fps_glreset()` alongside the other modules' | render |
| `tagpu_menu.c` `commit_one(R_FPS)` | records `fps=` in the settings store; the store's own file is written from `tagpu_menu_present`, off the game thread | game (the click), render (the write) |

**Settings.** The row is `fps=` in `impure.cfg` ([renderers](../renderers.html) §2.10b), read through
`tagpu_settings_fps()` every 30 frames on the render thread. `tagpu_fps.on` is its lever: present,
it turns the readout on whatever the row says, and greys the row.

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

**Since G19d it is also the FIRST pass with a Vulkan edition** ([§2.26](log-g19-vulkan-port.md#226-the-fps-readout-drawn-by-vulkan-tagpu_vk_fpsc-tagpu_vk_passh-tagpu_vk_shotc-phase-g-g19d)),
and two things about this file changed for it. `tagpu_fps_quads` hands the frame's vertices over
**exactly once** — the freshness rule, since the overlay does not run on every path that reaches
the swap and a frame it skipped must not redraw the last one's text. And `tagpu_fps.ab` captures
one frame of the readout over a black field to `tagpu_fps_gl.ppm`: an **oracle** rather than
instrumentation, because a readout drawn over the game cannot be compared against anything with
the game under it. It clears the whole frame on purpose, once, until the lever is taken away.

### 2.15 HUD scale (`tagpu_hud.c`, **off unless armed**, `tagpu_hud.on`) — Phase F G18f

> **Both halves draw on Vulkan.** The magnification is the UI composite's `uHud`
> (`tagpu_gui_surf.c`'s shader, recorded by `tagpu_vk_gui.c`), and the world target is placed
> on the frame shifted by `tagpu_hud_shift` (`tagpu_native.c`, where it publishes
> `TAGPU_WORLDTGT`). It is not a play default; the Visuals row's `hudscale=` turns it on.

The in-game HUD magnified inside the player's own Screen Size, over a world the engine goes on
drawing exactly as it always did — so Screen Size and HUD size are two dials rather than two
names for one number. Design and every decision behind it: [GUI renderer](../gui-renderer.html)
§22, and **§22.5 for why the first build was withdrawn the same day**; the geometry it rests
on: [resolution](../resolution.html) §3.4a; the engine reading: [engine
map](../exe-reverse-engineering.html) *The viewport rect at game entry* and *The world→screen
projection is NOT derived from the viewport rect*.

**It writes no engine memory at all**, and that is the whole of the correction. The first build
reserved the space by writing the viewport rect; `L`/`T` are the screen→world origin inside
`0x498DA0` and nothing else, while TA's world→screen projection is a `+0x80`/`+0x20` pair of
baked immediates, so the world tore in two by `((s−1)·128, (s−1)·32)` — measured, 1024×768
Auto: the engine picked a unit 76 px left and 19 px up of where it was drawn. The HUD now
covers the outer world instead of asking for it.

| site | what we do there | thread |
|---|---|---|
| the UI composite (`tagpu_gui_surf.c`'s shader, `tagpu_vk_gui.c`) | `uHud` — the two integers, `1/s` and `s`. Three regions sample the twin at `s` texels per device pixel; the ramp widens by `s` with them; the world region is sampled back by the shift. The three regions are the side panel and the two bars; the shader's uniform block is 80 bytes. Inert at `s = 1` | render |
| `mouse.c`, `winapi_hooks.c` ×4, `wndproc.c` ×3 | `tagpu_hud_to_engine()` at the end of every client → game conversion: inside a HUD region the engine is handed the point on its own 1× HUD grid | message |
| `sharp_cursor`, `sharp_minimap` | `tagpu_hud_to_screen()` — the engine's own cursor position (the fallback path only) and the minimap's box go the other way, so the sharp layer lands on the magnified art | render |
| `tagpu_menu.c` | the "UI scale" row, `trigger_rect()` and `panel_rect()` | game / window |
| `0x4288D0` (observer), gated on return address `0x498242` | write `R`, `B`, `viewW`, `viewH` so the engine's viewport IS the visible window — `L`/`T` untouched, because the projection bakes them. Writes nothing at stock | game |
| `tagpu_native.c`, the `TAGPU_WORLDTGT` publish; `tagpu_vk_world_record` | the one draw that puts the world target on the frame: its rect is shifted by `(128s−128, 32s−32)` × k device px; the viewport keeps the whole block and only the scissor is clipped to the window, so the block's far edges (the engine's right inset and bottom bar, no world in them) fall off the frame instead of squeezing the world. Without it every world thing — the build square, a unit under a click — sits the shift away from the pointer, a constant in screen px that reads as "far" at zoom < 1, where the square is small. **Not covered**: when the target is refused (`tagpu_vk_world_prepare` returns 0 — a slot that will not size, a target over `WORLD_MAXDIM`) the world passes draw straight into the swapchain with no shift, and the `scaffold.on` instrument draws over the whole frame unshifted | render |
**One resolver, so the two halves cannot disagree.** `tagpu_hud_geom(W, H, pct, …)` is a pure
function that clamps to the screen's own ceiling and yields `s`, the panel width and the bar
height. The composite and the pointer map both call it; neither owns the answer, and because
`128s / s` is exactly `128`, the screen column where the panel ends is exactly the engine
column where the world begins. The ceiling is `H/480` — [resolution](../resolution.html) §3.4a
measured the panel to be a fixed 128×480 block that does not stretch — with a second bound
that keeps at least 256 px of world width.

**Fields we write.** Four of the six viewport ints at `main+0x37E2F..0x37E3B` (`R`, `B`, `viewW`,
`viewH`), once per game entry and only when the resolved scale is past stock. **`L` and `T` are
never written** — that is the whole lesson of §22.5. Because viewW/viewH size the SORT buffers at
LoadMap, the setting is game-entry-time: a scale chosen mid-game waits for the next game.

**Why a stale scale is safe.** One word crosses threads, the percentage in force, which every
consumer re-resolves against the screen *it* sees, so either value a racing 32-bit read can
return is one that fits that screen. The worst a mid-frame change can do is leave the pointer
map and the picture one frame apart, and neither is engine state. The shell needs no signal of
its own: its surface is atom-locked at 640×480 whatever Screen Size says, and 640×480's ceiling
is exactly 1.0.

**Files.** `tagpu_hud.on` (`scale=auto` / `scale=<percent>`) and `tagpu_hud.off`, written as a
pair for the reason §2.8 gives.

**Known costs.** The HUD is 1× art magnified: bigger, not sharper. That is now the only one:
§22.6 made the engine's viewport the visible window, so the whole map is reachable, the camera
centres on what the player sees, and the world under the HUD is no longer drawn at all.
