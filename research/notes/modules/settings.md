# Play defaults, settings and the render-options screen

The play defaults, the settings store, the options screen and the test-launch registry.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.8 The play defaults (`tagpu_opt.c`, `tagpu_defaults.off`) — since 2026-09-08

Every pass is armed by a file beside `TotalA.exe`, `tagpu_<x>.on`, whose contents are its tokens.
**With no file at all, the play set is on.** A pass that is also a menu row (only
`tagpu_classicpp.on`, the Renderer row) asks the settings store in between: its own `.on`/`.off`
first, then `impure.cfg`, then this table ([renderers](../renderers.html) §2.10b). `tagpu_opt.c` answers two questions for the fifteen files below — *is the pass on*
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

Nothing on it changes the simulation: the extra weapons are always on, in the fail-closed table
([extra weapons](../extra-weapons.html), *Layer 1*). `tagpu_owndraw`, `tagpu_featown` and
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
| `0x46A308` (observer, post-GUI) | blit the sprocket into the back buffer with `CopyGafToContext 0x4B7F90(NULL, frame, x, y)` | game |
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

**The front-end screen carries fifteen rows in two columns.** `VISUALS.GUI` is re-emitted into
the same `.ufo` with five of the stock gadgets moved (`GAMMA`, `VIDSLDR` and their three
captions; names, `assoc`, `commonattribs`, `range` and `stages` verbatim), the stock `SHADING`,
`ANTI` and `BSHADOWS` toggles removed, and thirteen rows of our own:

| column | rows |
|---|---|
| **Window** | Display mode (window / borderless fullscreen, `util_toggle_fullscreen`), Monitor (`EnumDisplayMonitors`, `SetWindowPos`), UI scale (Auto / 1x..4x, the client set to k x the Screen Size row's own mode at `main+0x37F1B/+0x37F1F`), Screen Size (stock `VIDSLDR`), Vsync (on / off: the present mode and the frame limiter's one cap; a row of the in-game panel's table shown in this column — renderers §2.10c), Gamma (stock), **GPU (Vulkan)** (G19b — `tagpu_vk.h`; caption at y 364, control at 380, in the space the Gamma slider left free) |
| **Impure rendering** | Renderer, Undithered assets, Dynamic lighting, Shadows, Shadow quality, Supersampling, FPS counter, Map edge |

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
(`TotalA.exe`'s or `win32.dll`'s), or a `win32.dll` not loaded at attach (a static import of
`TotalA.exe`, so the loader maps it before any `DllMain` runs) ends the process with `TerminateProcess` at attach, after one
log line: `registry: TEST MODE, entered by <signal>, but <what>: the game is not run`. The game's
first instruction never runs. The signal in every line names the module whose attach it ends
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
| `win32.dll` | `RegOpenKeyExA`, `RegQueryValueExA` | its own; it reads `musicvol` and `cdmode` |

After patching, `rs_count` walks the import directories of the exe and of `win32.dll`: every
ADVAPI32 import whose name begins with `Reg` must now hold one of the hooks, or the process is
terminated with a log line before the game's first instruction. The fork's `hook_init`, which runs later, hooks no
registry function, so nothing overwrites these slots.

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

**The guarantee, and its edge.** TA's settings key is never written in test mode: the imports
above are the only way `TotalA.exe`'s and `win32.dll`'s code reaches the registry, and the `-r`
switch is closed. Nothing hooks the system DLLs the game uses and loads by name; the other
DLLs it loads at run time (`online.dll`, the extension DLLs `online.dll` loads into the game's
process, `reporter.dll`, `DebugHelper.dll`); the programs the game starts (what `ShellExecuteA`
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
the handle range; the `-r` jump-table patch and its fail-closed mismatch; `win32.dll`
required at attach (a static import of `TotalA.exe`).
