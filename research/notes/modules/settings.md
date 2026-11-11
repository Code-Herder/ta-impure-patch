# Play defaults, settings and the render-options screen

The play defaults, the settings store, the options screen and the test-launch registry.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.8 The play defaults (`tagpu_opt.c`, `tagpu_defaults.off`) — since 2026-09-08

Every pass is armed by a file beside `TotalA.exe`, `tagpu_<x>.on`, whose contents are its tokens.
**With no file at all, the play set is on.** A pass that is also a menu row
(`tagpu_classicpp.on`, the Renderer row; `tagpu_mirror.on`, Map edge; `tagpu_formation.on`,
Formation move) asks the settings store in between: its own `.on`/`.off` first, then
`impure.cfg`, then `tagpu_defaults.off`, then this table ([renderers](../renderers.html) §2.10b).
`tagpu_opt.c` answers two questions for the files below — *is the pass on*
(`tagpu_opt_on`) and *what are its tokens* (`tagpu_opt_read`) — and every reader of those files,
at attach and on its per-frame poll, asks it instead of the file system. Nothing else changed:
the parsers, the polls, the `*own` install-at-attach rule.

| file | default tokens | only with |
|---|---|---|
| `tagpu_native.on` | `all wrecks` | |
| `tagpu_terr.on`, `tagpu_terrown.on` | | `terrown` with `terr` |
| `tagpu_feat.on` | | |
| `tagpu_fx.on`, `tagpu_sfx.on` | | |
| `tagpu_mark.on`, `tagpu_markown.on`, `tagpu_order.on` | | `markown` with `mark` |
| `tagpu_ghost.on` | | `native` |
| `tagpu_zoom.on`, `tagpu_vpwide.on` | | `vpwide` with `zoom` |
| `tagpu_mirror.on`, `tagpu_gui.on`, `tagpu_classicpp.on` | | |
| `tagpu_formation.on` | | |
| `tagpu_recorder.on` | | |

Nothing on it changes the simulation: the extra weapons are always on, in the fail-closed table
([extra weapons](../extra-weapons.html), *Layer 1*). The formation move is input: it chooses where
each selected unit is sent and gives those orders through the engine's own roots, as a click does
([formation](formation.html)). `tagpu_owndraw`, `tagpu_featown` and
`tagpu_fxown` are not on it either: each stops the engine drawing something, which holes the
reference frame the golden source is taken from (`tagpu_opt.c` has the argument and the numbers).

The rules, in precedence: a `tagpu_<x>.on` that exists wins, tokens and all, as ever; a
`tagpu_<x>.off` (and no `.on`) turns a default-on pass off; `tagpu_defaults.off` turns the table
off — every pass opt-in again. The *only with* column is the pairing `tacli launch` makes when it
auto-arms the `*own` half, and the same rule for the two rows that depend on another: `native.off`
takes `ghost` with it, and `zoom.off` takes `vpwide`, because the repair `vpwide` carries is what
the zoom cannot go live without (§2.3b). Everything off the table — the instrumentation triggers,
the knob files `tagpu_restoreglsl.on` / `tagpu_classicpp.cfg` (and `tagpu_hires.on`, read by nothing since the replacement pipeline's deletion), the `.off`
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

### 2.8b The fork's settings (`tagpu_cfg.c`) — no ddraw.ini

**The DLL reads no config file.** `cfg_load()` (`config.c`) sets every `g_config` field to
cnc-ddraw's own default — the value it took when the key was absent — and `tagpu_cfg_defaults()`
then sets the ones TA needs, attaches nothing else, and logs one line:
`cfg: max_resolutions 90, toggle_borderless, lock_surfaces, singlecpu off, maintas;
display=… vsync=… window=…; renderer Vulkan|GDI (tagpu_gdi.on)`. Upstream's template writer,
`ini.c`, the per-game section lookup and `cfg_save`'s ini half are deleted, and so are the eleven
per-game hacks (`vhack`, `tshack`, `infantryhack`, …), none of which TA ever turned on
([removing ddraw.ini](../ddraw-ini-removal.html)).

| field | ours | why |
|---|---|---|
| `windowed` + `fullscreen` | both true, then the store's `display` | borderless fullscreen, or a window; never `fullscreen` alone, which is `util_toggle_fullscreen`'s exclusive modeset |
| `toggle_borderless` | true | alt+enter switches borderless ↔ window instead of taking the exclusive branch, a real `ChangeDisplaySettings` |
| `max_resolutions` | 90 | the mode list TA is fed; **bounded at 100 by a static assert**, because TA's "DISPLAY MODES" allocation is 100 entries whose writer `0x4B5330` does not bounds-check ([resolution](../resolution.html) §6.1/§6.3), and cnc-ddraw's 0 means no cap |
| `lock_surfaces` | true | `dds_Lock`/`dds_Unlock` hold the surface's critical section, and the GDI backend the primary's while it blits, only when set |
| `singlecpu` | false | cnc-ddraw's default pins the whole process, the render thread included, to one CPU |
| `maintas` | true | the aspect-preserving fit |
| `window_rect` | the store's `window` | read under `tagpu_defaults.off` too (`tagpu_settings_window`): nothing else can place the window. The store's `vsync` is read the same way, every frame, and is no `g_config` field ([renderers](../renderers.html) §2.10c) |
| `center_window` | never, when the store placed the window | at cnc-ddraw's `auto`, the shell-to-game mode switch re-centres the window off its tile; a placed client with a corner on no monitor is moved, decoration included, into the work area of the monitor it overlaps most instead (`dd_SetDisplayMode`) |
| `gdi` | `tagpu_gdi.on` present | forces the GDI backend; otherwise Vulkan |
| `inject_resolution` | the desktop mode | the one list entry exempt from the `CDS_TEST` filter, so the monitor's own mode is *guaranteed* into the picker ([resolution](../resolution.html) §6.5); filled lazily in `EnumDisplayModes`, because `cfg_load` runs under the loader lock and must not touch the display |

**tacli writes the placement into `impure.cfg`** on every launch (`write_placement`:
`display=window`, `window=<tile>,<client>`, `vsync`), which is what keeps an instance on its
tile, and `tacli launch --shipped` writes none of it, for the player's path.

### 2.12 The render-options screen (`tagpu_menu.c`, `tagpu_ufo.c`, on by default, `tagpu_menu.off`) — Phase F G18

The in-game settings screen. Design and the decisions behind it:
[renderers](../renderers.html) §2.10; the engine reading: [engine map](../exe-reverse-engineering.html)
*The screen lifecycle*, *A screen's own GAF*, *Setting a gadget's state*, *Where the engine
looks for archives*. It is a **real TA `.GUI` screen**, so the engine does the hit-testing,
the dispatch, the plate art, the fonts and the save-under, and the G15/G17 twins carry it for
free; what we supply is two bytes of gadget state, one frame's pixels, one 28×28 trigger and
one small archive.

| site | what we do there | thread |
|---|---|---|
| `DLL_PROCESS_ATTACH` | write `impure-patch.ufo` unconditionally, with a version stamp: six files — the panel (`impure/render.gui`, `anims/render.gaf`), the front end's `impure/visuals.gui` + `anims/visuals.gaf`, and the in-game `impure/visualrt.gui` + `anims/visualrt.gaf`, which replaces the stock screen — or a mod's — with Gamma, Restore and Undo. The four Visuals files go in only when the dialog-build observer can arm — not under `tagpu_menu.off` — and the redirect below sends GUI_Load to them only once it has, since they drop the stock toggles and route Restore/Undo to us. `DDRAW.dll` is TotalA.exe's first static import, so this precedes `InitTAHPIAry 0x41D4C0`'s `*.UFO` glob by the loader's rules | — |
| `0x4AA99F` (observer in `GUI_Load`, no return hijack) | **the redirect**: for `RENDER.GUI`, and for `VISUALS.GUI` / `VISUALRT.GUI` while their handlers are in, rewrite GUI_Load's local copy of the gui prefix (`[esp]`, checked to be `esp+0x38`) to `impure\`, so our screens load from a folder only our archive has — whatever a mod calls its gui folder (`guiE`, `guiM`, `guiP`) and whatever it ships there. `gi+0x9B6` itself is never written. It lands only after the archive was written, and nothing of ours is armed without it: GUI_Load faults on a missing file | game |
| `DrawGameScreen 0x468CF0` (observer) | the per-frame tick: sample the trigger file on its edges, open or close, re-assert `main+0x37EA0`, and recover if the screen was freed under us | game |
| `0x46A308` (observer, post-GUI) | blit the sprocket into the back buffer with `CopyGafToContext 0x4B7F90(NULL, frame, x, y)` -- on GDI, or while the wiki cannot draw it; otherwise blit nothing and leave the sprocket to the wiki's layer, up or down (`tagpu_menu_bar_cog`, carried by the same frame's wiki record) | game |
| `GUIMEMSTRUCT+0x08` (our `OnCommand`) | advance the row, `GUIGADGET_SetStatus 0x4A1080` for every row, `grayedout` for Shadow quality, set the repaint flag `gi+0xCCA` via `0x49FA90`, and **answer the pump with `0x4AB0A0(gi)`** (`gi->UIChange_f = -1`). **It writes no file** | game |
| `tagpu_shield.c`, both paths | `tagpu_menu_click()` — the sprocket's hit test, from `deliver_mouse()` (injected) and from the wndproc before the shield's gate (real) | game |
| `tagpu_zoom.c`, two entry points | `tagpu_menu_owns_point()` — the zoom transform must leave a point the menu owns alone. The panel hangs over the world and the transform's gate is geometric, so at any zoom ≠ 1 a row click was bent away and **no row worked**; `tagpu_zoom_drop_mouse` must not treat it as the display-only ring either | game |
| `render_vk.c`'s and `render_gdi.c`'s frame | `tagpu_menu_present()` — the settings store's write (`tagpu_settings_present`) | render |
| `0x42F9A0` (observer, the registry loader) | after the startup load: push the store into the engine's Visuals fields (Gamma, the option word, the screen size). Around every later load: snapshot the fields before it and put them back after it, so a reload never changes an owned value. Installed only when the Visuals screens' observer is (so not under `tagpu_menu.off`), and never under `tagpu_defaults.off` | game |
| `0x45CAE0` (observer, the stash restore) | after it: push the store again, so CANCEL's copy of the stash cannot bring back a value the store no longer holds. Optional: if it will not install, logged and the rest stands | game |
| gadget `+0x144` of `GAMMA` and `VIDSLDR` | the slider callbacks `0x45BD20` / `0x45BBF0` are wrapped, per visit and only while the field still holds the engine's own: the wrapper calls it, then records the value it wrote into the store | game |
| open | `GUI_Load 0x4AA8F0(gi, main+0x37EA0, flags)` with `0x20` + `0x400`, patch the panel rect, set `+0x08`/`+0x0C`, then `0x4C2470(); GUI_StageUpdateDraw(gi, 0x21); 0x4C2870()` — GUI_Load's own suppressed stage 1, reproduced; then repaint the ground and set `gi+0xCCA` | game |
| close | restore `main+0x37EA0`, then `GUI_Pop 0x4A9660` when our screen is on top, else `UpdateIngameGUI 0x491D70(1)` | game |
| teardown wrap's entry (`tagpu_reclaim.c`) | `tagpu_menu_level_end`: restore `main+0x37EA0` and forget the slot's screen without a pop | game |

**Answering the pump is not optional** [VERIFIED 2026-09-11]. `gi->UIChange_f` is
bidirectional: the pump writes the actuated index into it before calling `OnCommand`
(`0x4AA675`) and reads it back afterwards (`0x4AA79A`), and a handler that returns with it
still set is asking to be popped — whereupon `0x4AA7BC..0x4AA7FA`, an **inlined copy of
`GUI_Pop`'s body**, relinks the stack and `free()`s the `GUIMEMSTRUCT`. Every engine handler
ends with `0x4AB0A0(gi)` for exactly this reason; see *The pump's dispatch contract* in the
[engine map](../exe-reverse-engineering.html).

**The front-end screen carries eighteen rows in two columns.** `VISUALS.GUI` is re-emitted into
the same `.ufo` with five of the stock gadgets moved (`GAMMA`, `VIDSLDR` and their three
captions; names, `assoc`, `commonattribs`, `range` and `stages` verbatim), the stock `SHADING`,
`ANTI` and `BSHADOWS` toggles removed, and sixteen rows of our own:

| column | rows |
|---|---|
| **Window** | Display mode (window / borderless fullscreen, `util_toggle_fullscreen`), Monitor (`EnumDisplayMonitors`, `SetWindowPos`), UI scale (Auto / 1x..4x, the client set to k x the Screen Size row's own mode at `main+0x37F1B/+0x37F1F`), Screen Size (stock `VIDSLDR`), Vsync (on / off: the present mode and the frame limiter's one cap; a row of the in-game panel's table shown in this column — renderers §2.10c), Gamma (stock), **GPU (Vulkan)** (`tagpu_vk.h`; caption at y 364, control at 380, in the space the Gamma slider left free), **Mega map wheel** (Ctrl+wheel / Wheel, `wheel=` — which wheel engages the mega map, [camera and zoom](camera-zoom.html) §2.3m; a row of the in-game panel's table, caption at 404 and control at 420, the column's last slot: the Impure options column is full and the shell screen ends at 480) |
| **Impure options** | Renderer, Undithered assets, Dynamic lighting, Shadows, Shadow quality, Supersampling, FPS counter, Map edge, Resource snapping, Quick mex/geo |

Four things this rests on, each measured rather than assumed:

- **Display mode, Monitor and UI scale are applied on the thread that owns the window.**
  Each ends in a window call, and a cross-thread one is a wait on a message pump rather than
  a visible error, so the click POSTS `WM_TAGPU_DISPLAY` and the wndproc does the work — the
  same contract `tagpu_shield.c` uses for injected input. The GPU row rides the same message
  and touches no window.
- **`util_toggle_fullscreen` does not restore the window size on the way back** (measured:
  1024x768 -> 3840x2160 -> 3840x2160), so the Display mode row saves the windowed client
  before leaving and puts it back itself.
- **The monitor is a rect, never a display "mode".** On a multi-monitor X server wine
  reports the VIRTUAL DESKTOP as the current, registry and largest-enumerated mode, for
  every adapter alike — 6200x2160 on the reference setup's three outputs. So the Monitor
  row, the borderless-fullscreen size AND position, and the Screen Size list all come from
  `util_target_monitor` (`utils.c`), which asks `GetMonitorInfo`. The measurement, and the
  per-monitor mode lists it produces, are in [resolution](../resolution.html) §6.6.
- **The ground is ours.** STARTOPT's background paints one column of recess bars across
  x 267..405, drawn for a single centred column; two columns cannot sit in it and it is
  the game's art. So the screen carries one `id=12` ground frame (270x420 at (200,54)) in
  `anims/visuals.gaf`, with its own recesses, covering those bars.
- **The GPU row's list is one launch behind, by construction** [G19b, 2026-09-15]. Its captions
  have to be inside this generated `.GUI`, and `tagpu_menu_init` writes the archive at DLL
  attach — where creating a Vulkan instance (and so loading an ICD) is exactly the
  `LoadLibrary`-from-`DllMain` [field notes](../field-notes.html) forbids. So a worker enumerates
  the devices once the render thread is up and writes `tagpu_vk.gpus`, and the menu reads that
  cache at the *next* attach. It is the same bargain the Monitor row already makes for a
  hot-plugged monitor. The row's **model** is not one launch behind: the choice is stored by
  name as `gpu=` in the settings store (`impure.cfg`, [renderers](../renderers.html) §2.10b), and
  its first stage is **Auto** (`gpu=auto`, the lane's own ranking: [renderers](../renderers.html)
  §2.10b). A software rasteriser is not listed while a GPU is present, so on a machine with one
  GPU the row is Auto alone, greyed. Auto plates Auto; a named choice plates `tagpu_vk_gpu_active()` — the device the render
  thread actually bound — whenever the lane is up, so a request that could not be honoured shows
  as the device that was.

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

**Restore Default and Undo Changes are ours, and not forwarded.** Both are STARTOPT's buttons,
and the engine's branches would reset the engine's options from its own defaults or from the
Options stash — values the store owns now. So `OnCommand` handles both and does the engine's own
ending itself, `GUI_Pop` + `0x45E5E0(0)`, with `s_visKeep` set so the rebuild seeds the plates
from the model rather than re-reading levers the deferred write has not reached yet. **Undo**
restores every row the screen opened with: the rendering rows, Gamma and Screen Size to the
store's values at open, and the Window rows the visit touched — Display mode and Monitor
included, since it is the escape hatch for a mode the player cannot see the menu on. **Restore**
sets the rendering rows to the Classic++ defaults, UI scale off, the cap to 60, Gamma 12 and the
screen size to native, clears DitheredFog as the engine's own Restore did, and deliberately
leaves Display mode and Monitor alone: a default that moves the window to a monitor the player
cannot see would hide the button that undoes it. The in-game `VISUALRT` screen has the same two
buttons for its one row, Gamma.

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
`gi+0xCCA`, the repaint flag. The loaded GAF frame's colour plane (`+0x10 PtrFrameBits`),
repainted in place with the composed ground. The `+0x144` callback of `GAMMA` and `VIDSLDR`.
And the engine's Visuals options — the option word `main+0x37F06` (bits 1..5), Gamma
`main+0x37F08` and the screen size `main+0x37F1B/+0x37F1F` — from the settings store (§2.5).
**Nothing sim-side, and nothing that replicates** beyond what stock already sends: the
battleroom advertises the screen size to the other players, as it always has.

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

**Shadows is exempt from `Custom` the same way.** The row drives the engine's own shadow bits
(2, 3 and 4 of `main+0x37F06`) as well as the Classic++ shadows, so it is a switch a Classic player
needs too: the preset leaves it alone, it never greys with the lane, and changing it never makes
Renderer read `Custom`. For the same reason it is the one row **not** greyed under
`tagpu_defaults.off`: there it plates the engine's bit 2 and sets bits 2–4 directly, the stock
`BSHADOWS` write, and Restore and Undo are forwarded to the engine's branches again.

The in-game panel is 496×388: **two columns of `COL_W` 248** (`PANEL_W` = 2·`COL_W`), twenty-five
rows on a 24 px pitch filling the left column first (`row_pos`: `COL_ROWS` = `(R_COUNT + 1)/2` =
13 on the left, 12 on the right), each row a label at `x0 + 12` (112 wide) beside a control at
`x0 + 128` (108 wide), `x0` its column's left edge. The left column's last row (Mega map wheel)
starts at `ROW_Y0 34 + ROW_PITCH 24 × 12 = 322` and ends at 340; a row's recess is
`ROW_H + 2·PAD` = 22 px, leaving 2 px of face between two. The floor rule `DIV_BOT` sits 8 px
under the last row, at 348, the Resource profile caption under it at 356, and the bottom bolts at
`PANEL_H − 10`. **The height follows the left column's count**: `DIV_BOT` is
`ROW_Y0 + ROW_PITCH·(COL_ROWS − 1) + ROW_H + 8` and `PANEL_H` is `DIV_BOT + 40`, and a
compile-time check (`menu_panel_fits_480`) refuses a build whose panel, hanging from engine row
`BAR_H` 32, would end past row 480 — thirteen rows a column end at 420, fifteen would end at 468.
**The width keeps it off the side bar**: a second check, `menu_panel_clears_side`, requires
`SIDE_W 128 + PANEL_W + MARGIN ≤ 640`, so at 640×480 the panel covers x 128..623 and rows 32..419
and never reaches the side bar. The reason is a strip of the side bar, between the minimap and the
ORDERS buttons, that belongs to no GUI window: the engine draws it once a level and never again,
so a panel over it leaves its pixels there after it closes — MEASURED 2026-11-09 at 640×480 with
304-px columns, two labels stayed through the close, a select and a deselect, and the engine's
own surface (`tacli shot`) showed them too. The generated `.GUI` is written into a 24 KB buffer;
the twenty-five rows take 16 190 bytes (the log's `rows=25 gui=16190`, 2026-11-11). *Formation move*, *Mega map zoom
in*, *Radar dots* and the ten range-ring rows are this panel's alone: the front-end screen's two
columns are full (`vis_shown`), so a player sets them in game or in `impure.cfg`. *Radar dots* (Zoomed out \|
Always) is `radardots=` (`TS_RADARDOTS`, `zoomed` or `always`, default `always`): a radar
contact's dot in the world view at every level, or only where the strategic icons come in
([strategic icons](icons.html), *Radar dots*); read every frame by the render thread and every
hover query by the game thread, never greyed with the lane, held under `tagpu_defaults.off`,
where it reads Zoomed out, and set to Always by Restore defaults.
`PANEL_W` and `PANEL_H` size the GAF frame header, the ground, the ramp, the border and the corner
bolts, and the panel is **composed at runtime** from the player's install rather than shipped, so
no art is regenerated. `tools/guipanel.py` draws a six-row panel 304×212; the columns, the row
pitch and the size are `tagpu_menu.c`'s alone.

**The strategic icons' three keys have no row on either panel** — `iconscale`, `icondist`,
`iconfade` in `impure.cfg` ([renderers](../renderers.html) §2.10b, [strategic icons](icons.html)).
The keys are set in the file and read live.

**The weapon range rings' rows** ([range rings](range-rings.html), *Settings*), the right column's
last ten, in game only: *Range rings* (Off \| On, `ranges`; Off brings the engine's `ShowRanges`
circles back), *Attack rings* (Always \| Shift held, `rangeshift`), *Cursor range* (Off \| On,
`rangecursor`), *Sensor ranges* (Off \| On, `sensorview`: the radar, sonar and jammer coverage of
the viewed player and its allies, always), *Sensor preview* (Off \| On, `rangesensor`: the
coverage of a building being placed), and one row a defense class — *Ground*, *Air*, *Nuke*, *Cannon* and *LRPC
defense* — cycling None \| Enemy \| Allied \| Both: stage bit 1 is the class's bit of
`rangeenemy`, bit 2 its bit of `rangeally` (`RCLASS_BIT`), and a click on any of the five writes
both masks whole from all five rows. The seven rows of the rings' own options — *Attack rings*,
*Cursor range* and the five classes — grey while *Range rings* is Off; the two sensor rows do not,
since the sensors are drawn whether or not the rings are. Restore defaults sets BAR's: rings on,
Always, cursor on, enemies' ground, air, nuke and cannon, allies' nuke; and the sensor view and the
sensor preview on. In the file the masks are comma lists of `ground`, `air`, `nuke`, `cannon`, `lrpc`
or `none` (a word that is not a class keeps the default, logged), and
`rangetoggle=NAME:n:ally:enemy,...` is the Alt+. / Alt+, map, kept by unit name like
`placespacing` (at most 64 types, `n` 1..16), which has no row. The game thread reads every key
every frame; under `tagpu_defaults.off` the store is ignored, the rows are held at the compiled
defaults, and the rings and both sensor keys are on.

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
drawn. It is 28×28 at `(w−16−28, 2)`, sharing `MARGIN = 16` with the panel at `(w−16−496, 32)`
so the two right edges land on one line, at `w − 16`; the panel's left edge is 512 at 1024 and
1408 at 1920.

**Who draws it.** On GDI the engine's blitter draws the code-made sprocket (`sprocket`,
`build_trigger`: eight teeth in palette indices, normal, hover and pressed). On the Vulkan lane
the wiki's layer draws the kit's `config_emblem` in the same 28-bar-pixel slot, the emblem the
wiki and rewind buttons share, `config_emblem_down` while the sprocket is held or the panel is open
(`wiki_button`, `tagpu_wikilay.c`), and the observer blits nothing -- but only while the last
published packet carried a wiki record (`tagpu_wiki_shown`), the HUD's pass stands, the wiki has
not refused and its art is loaded. The skip is taken inside `DrawGameScreen` and every wiki record
filled after it, by the same thread after the same draw, carries it (`TAGPU_PK_WIKI.config`).
Every state that keeps the wiki off the screen for good fails one of those tests -- a refusal of
the HUD's pass or of the wiki (its layout, or a kit the pass has no room for), a publisher that
fills nothing, records that do not fit the packet -- so the engine draws the sprocket again from
the next draw. Only drawing moved: the hit test is still `tagpu_menu_click` on this rect, and the
wiki puts no control there. Gaps: the emblem has no hover state (neither has the wiki button nor
the rewind button); the engine's draws for a screenshot or the movie recorder (`0x495C76`,
`0x495E66`, `0x4962C2`) leave the sprocket out on the Vulkan lane; and the two halves are not
paired on the screen -- the blit reaches it through the GUI's op ring and the skip through the
frame packet, which publishes only fresh frames -- so at a change of state (a press, the panel
opening, a refusal, the session's first in-play frame) the screen can show both sprockets or
neither for one frame.

**Known cost.** The engine rebuilds the whole in-game GUI stack on a world click, and the
panel hangs over the world, so clicking a row costs one frame of the panel being re-pushed.
The row's own state survives it (the model is ours; the levers are read only on the player's
open), but the rebuild is visible if you look for it.

### 2.92 TotalA.exe's registry in a tacli test launch (`tagpu_regstore.c`, test mode only) — G21c

The plan is [hardware portability](../hardware-portability.html) §3 G21c, the contract is
`tagpu_regstore.h`, and the engine's side, every call site with the keys and values it reads
and writes, is [exe reverse engineering](../exe-reverse-engineering.html) §"The registry".

**Every tacli launch of a DLL that has this module runs in test mode, local ones included** (a
`--keep-dll` build from before it runs on the shared registry, and tacli warns). A remote
instance's test folder holds `tacli-state\registry.txt` so the player's key is never written; a
local instance's gamedir holds `tacli-state/registry.txt` because every local prefix's
`user.reg` is one inode, so the store is what makes TA's key the instance's own ([tacli design](../tacli-design.html) §"The
registry: a store per instance"). Under wine `GetModuleFileNameW(NULL)` names the gamedir,
although `TotalA.exe` there is a symlink into the Steam install: a local launch logs `entered by
the -xtacli-test token and the tacli-state folder beside TotalA.exe` [MEASURED 2026-09-26, wine
9.0].

**Test mode has two signals, and either is enough.** `tagpu_regstore_decide`, which `DllMain`
attach runs right after the fork's `delay_imports_init` and before the return for cnc-ddraw's
config tool (so an inherited `cnc_ddraw_config_init` cannot skip it; detach returns early
exactly when attach did), looks for the token `-xtacli-test` on the command line
and for a `tacli-state` folder beside the running exe (`GetModuleFileNameW(NULL)`, never the
working directory). tacli passes the token on every remote launch, and on every local launch
whose DLL serves the store; the engine skips it (`x` is
above the `'B'..'w'` of its switch table, `ja 0x49F461`, [command-line
options](../cmdline-options.html)). **Real mode needs both absent**: no token, and the folder not
found. **The store is served only with both**: `tagpu_regstore_init` refuses either alone. The
folder without the token (`… but no -xtacli-test token on the command line: a launch by a tacli
from before the per-instance store, or by hand -- launch it with the current tacli: the game
is not run`) is a launch whose values went elsewhere, into the registry every wine prefix
shares, which serving the store would drop without a word; the token without the folder has no
store. A folder that cannot be looked at (a share, an access rule), without the token, is real
mode too, so a player's folder stays inert. Real mode logs `registry: real (no -xtacli-test
token, and no tacli-state folder beside TotalA.exe)` and installs nothing, so a player's game
carries no hook of this section.

**Test mode fails closed.** A store that is missing, a folder, unreadable or not loaded whole,
no memory for it, an exe path that cannot be read, a registry import the hooks do not answer
(the exe's, or one of any DLL of its folder, whenever that DLL loads), or a loader that will not notify
ends the process with `TerminateProcess`, at attach or at that DLL's load, after one
log line: `registry: TEST MODE, entered by <signal>, but <what>: the game is not run`. At attach the game's
first instruction never runs; a DLL refused at its load ends a game that has started, in the same words. The signal in every line names the module whose attach it ends
(`s_exe`: `the tacli-state folder beside <module>`, `the -xtacli-test token on <module>'s
command line`), so a cnc-ddraw config tool opened in a test folder is refused in words about
itself and told to run from a folder with no `tacli-state`; only TotalA.exe's folder-alone
refusal says to launch with the current tacli. `rs_refuse_run` is `noreturn` by construction:
`ExitProcess` in a loop follows `TerminateProcess`, since a caller goes on as though the store
were whole.

**Import-table hooks, not engine addresses.** In test mode, at `DllMain` attach right after
`tagpu_log_init` (before `cfg_load`, the byte patches and the fork's `hook_init`),
`hook_patch_iat` replaces:

| module | imports | slots |
|---|---|---|
| `TotalA.exe` | `RegOpenKeyExA`, `RegQueryValueExA`, `RegCreateKeyA`, `RegOpenKeyA`, `RegQueryValueA`, `RegCreateKeyExA`, `RegSetValueExA`, `RegFlushKey`, `RegCloseKey` | `0x4FC000..0x4FC020`, one each in that order; every call site is in the engine map |
| every DLL loaded from the game's folder or below it, whenever and however | whichever registry functions it imports | its own |

`rs_hook_folder` decides by where a module was loaded from and never by a name or by who asked for it.
The retail exe's music DLL is `win32.dll`; a mod that renames the exe's imports renames it (Escalation's
exe imports `TAESC.dll`, `EMUSI.dll` and `EPLAYX.dll`, TA Zero's `ZDRAW.dll`, `ZMUSI.dll` and `ZPLAYX.dll`);
and the Patch Loader's `dplayx.dll`, as Total Mayhem and ProTA ship it, reaches `tplayx.dll` through
export forwarders alone, so no import table names it. At attach it walks the loader's own list
(`PEB+0x0C`, load order at `+0x0C`), under the loader lock, and then registers
`LdrRegisterDllNotification`: `rs_on_dll_load` hooks what loads later, once its imports are bound and
before its entry point runs, and logs `registry: test mode, <name> loaded: n of m registry imports
answered`. That order is checked each time, as hooks written before the binding would be overwritten
silently: an import slot still unbound ends the run (`rs_imports_bound`, below). MEASURED 2026-11-07 on
Windows 10 and Wine: opening the Multiplayer screen logs `taheatx`, `takalix`, `tamplayx` and `tawirepx`, each
`3 of 3`, none refused; the retail row's smoke requires it (`store_loaded`). That covers an export forwarder's target, the extension DLLs `online.dll` searches for and
anything else `LoadLibrary` brings in. The callback pins Impure's module, since it lives there. A library
outside the folder is left alone, so the real DirectPlay and winmm keep the real registry, and Impure's
own module is what answers.

No fixture and no retail DLL imports a registry function the hooks do not answer, and no DLL with no
registry import holds a `Reg*` name among its strings [MEASURED 2026-11-07 over every
DLL of the 20 fixtures and the Steam install]. A packed DLL hides both: `basscd.dll` (Total Mayhem, ProTA) is
Petite-packed, so `1 of 1` is its stub's `RegCloseKey`, and the stub looks up the rest through
`GetProcAddress`.

**Lookups are answered as imports are.** The store tables, at attach, every registry function the system
DLLs hand out (an export named `Reg*` and not `Register*`) by the address `GetProcAddress` gives for it:
ADVAPI32's, and kernel32's and KernelBase's, which export the same names — on Wine at addresses of their
own, thunks into KernelBase (`RegOpenKeyExA` at advapi32 `+0x60d4`, kernel32 `+0xd78c`, kernelbase
`+0x59320` in the reference setup's prefix); on Windows 10 forwarders that resolve to one address. Each
module of the folder, the exe among them, has every import slot set from that table: by the import's name
where a system DLL's descriptor has names (whatever the slot holds, so a shim's or another hook's wrapper is
replaced, as the fork replaces it), and by the slot's value everywhere else — a table with no names, an
import by ordinal, another DLL's name for the function. A registry function becomes its hook, and the real
`GetProcAddress`, kernel32's or KernelBase's, becomes the store's (`rs_GetProcAddress`). `tagpu_regstore_proc`
answers a lookup by the address it returns, so an ordinal or another DLL's name is answered alike: a
registry function looked up by code of the folder, or by code in no module at all, gets the store's hook,
`GetProcAddress` itself gets the store's, and a registry function the store does not answer ends the run.
Code of a module outside the folder gets the real one. The fork's `fake_GetProcAddress`, which `hook_init`
writes over the store's slot in every module whose import table has names, and Impure's own exported
`DDGetProcAddress` end in the same function, and in test mode the fork's revert at exit is skipped (the
module is pinned, so no slot outlives it).

`rs_count` then reads each module's slots back: every registry import must hold its hook, and every
`GetProcAddress` import the store's, or the process is terminated with a log line — at attach, before the
game's first instruction, or at that DLL's load. `rs_imports_bound` reads every descriptor: a slot still
holding its name table's entry, an RVA or an ordinal (the flag over 16 bits; a bound address above 2 GB in
a large-address-aware process has the flag and more) is unbound.

MEASURED 2026-11-08 on Windows 10, Total Mayhem and ProTA: `basscd.dll`'s unpacked code runs in memory of
no module and looks up `RegOpenKeyExA`, `RegQueryValueExA`, `RegEnumKeyA` and `RegCloseKey`, and before
this all four reached the real registry (among them an open of `HKLM\SOFTWARE\Classes\AudioCD\shell`
with full rights, now refused). The store answers `RegEnumKeyA` for it: a store key's subkeys are the
store's keys one level below it, sorted by name without case as Windows lists them; any other key's, the
prefixes `HKCU` and `HKCU\Software` included, are the real key's, read-only, as their values are. The
takeover's descriptor search skips the `GetProcAddress` slot as it skips the registry ones
(`tagpu_regstore_is_hook`): read as a binding, it made KERNEL32 "Impure's descriptor" on Escalation and TA
Zero, and TADR's `DllMain` ran on.

**What the hooks do.** `HKCU\Software\Cavedog Entertainment` and every key below it are served
from the store in memory; each change rewrites the file whole (a temporary file, flushed, moved
over it with `MoveFileExW`). `HKCU` and `HKCU\Software` get store handles that hold no values.
Every other key is read-only: an open asking for any right beyond reading, a create and a value
write are refused with `ERROR_ACCESS_DENIED`, and a read-only open, a query and a close go to
the real registry. No hook calls a registry function that writes. A store handle is
`0x6D5A0000 + 4 × index`, above every kernel handle and below the predefined keys.

**One byte patch, also test mode only**: the `-r` switch's two jump-table entries (§2.6), since
`dsetup.dll` writes the registry through imports of its own.

**The fork's own registry writes.** `indeo.c` writes four `vidc.iv*` values under
`HKCU\…\Drivers32` at attach and deletes them at detach; in test mode `dllmain.c` skips both
(the game plays Smacker and loads no Video for Windows codec). `debug.c` and `utils.c` only read,
and only in a `make DEBUG=1` build or under `age.dll`.

**State and threads.** No engine memory is read or written, so `tagpu_regstore.c` is not on
`thread-split.allow`. The store's keys and values sit under one critical section, which any
thread calling the registry may take. Under it run only this module, kernel32 file calls and
`tagpu_log`, whose own lock is a leaf. Real registry calls run outside it. A change of value
rewrites the file under that lock, on the calling thread: the game's saver writes about fifty
values in a burst, but only the ones that changed cost a rewrite (two rewrites in the logged
run under wine).

**The guarantee, and its edge.** TA's settings key is never written in test mode: once Impure has
attached, the code of `TotalA.exe` and of the folder's DLLs reaches the registry through its imports and
through `GetProcAddress` only by the hooks above, and the `-r` switch is closed. A DLL of the folder whose `DllMain` runs before that attach
meets the real registry during it: the attach line names them (`started before us:`, from the loader's
initialisation order, `+0x1C`). A Windows smoke row fails on any, but for the one that loads Impure on the
routes where TADR's `DllMain` does. MEASURED 2026-11-07 on Windows: `nothing of the
folder` on retail, `loader+tadr-ota`, Total Mayhem and ProTA, whose `tplayx.DLL 9 of 9` is the forwarder's target;
`TAESC.dll` on Escalation and `ZDRAW.dll` on TA Zero, which load Impure. Those meet no registry: in every
TADR build a setup loads before Impure (tadr-dev-ota's and v2026.8.6's `tdraw`, Escalation's `TAESC` and
tadr-dev-escalation's copy of it, TA Zero's 2025.4.24 `zdraw`, Twilight's v2026.6.8 `tdraw`), nothing that
runs before `LoadLibraryA("ddraw.dll")` returns reaches a `Reg*` import: not the C runtime's start-up, its
static-initialiser tables, its TLS callbacks (thread attach and detach only), nor `DllMain` up to that call,
which tests the reason, writes its log, and loads `dplayx` and then `ddraw` [DISASSEMBLED 2026-11-08: every
`Reg*` slot's call sites, direct and through a register, and the call graph from each of those roots,
indirect targets the reached code names included; the ddraw calls at `0x10040274`, `0x10033c34`,
`0x10035304`, `0x10046c54`, `0x1001d8e6`, `0x10031664`]. Their first registry use is in the part of
`DllMain` the takeover stops (tadr-dev-ota `0x100402fb`, `TAESC` `0x1003538b`). A TADR build not among
these is not read. Nothing hooks the system
DLLs the game uses and loads by name; the programs the game starts (what `ShellExecuteA`
opens, what `online.dll` starts); or Windows' own records (the Task Scheduler's of the task
while it exists, and those of the programs it runs). The full list: [tacli
design](../tacli-design.html) §"The registry: a file in the test folder".

**Measured** (the plan's G21c gate): under wine, `9 of 9 registry imports, win32.dll 2 of 2`,
77 distinct values written to the store and TA's section of the prefix's `user.reg` unchanged;
each fail-closed path (the token without a store, the store as a folder, the folder without a
store) ended at attach with its line and no registry call from `TotalA.exe` or `ddraw.dll`;
on the Windows test setup, TA's key and the Indeo key exported byte-identical before and after
a `scenario load`, with the counters `the real registry was asked for 1 read-only opens, 1
reads and 1 closes, and for no write ... 1 writes were refused`.

**Risky spots for a review.** The decision ahead of the config tool's return, the token's
parse of `GetCommandLineW` (the program name quoted or not), and the `TerminateProcess` inside
`DllMain`; the `PREFIX` handles and `RS_READ_SAM` (what counts as a read-only open);
`RegFlushKey` on a real key answering success without a call; the persistence under the lock;
the handle range; the `-r` jump-table patch and its fail-closed mismatch; the folder's static
imports required at attach. **The takeover reads these slots too:** a store hook in an ADVAPI32 slot
points into Impure's own image, and `tagpu_regstore_is_hook` is what keeps `to_descriptor_of` from
taking it for Impure's descriptor ([TADR exclusion](tadr-exclusion.md)).
