# Removing ddraw.ini

**Status: built 2026-09-24 (G18k).** The DLL reads, writes and creates no `ddraw.ini`, and the
release ships none. `impure.cfg` is the one settings file. This page is the record: what the file
did, what replaced each part of it, the owner's decisions, what was run to verify it, and the
gaps left open.

**Why.** A key in `ddraw.ini` won over the Visuals menu and greyed its row, and tacli wrote those
keys into every instance, so Display mode, Monitor and Frame cap could not be tested from a tacli
launch. The file was inherited, not needed: the fork is cnc-ddraw, whose whole configuration is
this file. Of the ten values the release shipped, three changed anything a player got
(`lock_surfaces`, `singlecpu`, `maintas`); the seven window keys served tacli.

## What the file did, and what does it now

| before | now |
|---|---|
| `cfg_init` found `ddraw.ini` (or `CNC_DDRAW_CONFIG_FILE`) beside the DLL and, if missing, **wrote upstream's ~1 700-line template** | nothing is opened or written; the template, `ini.c`, the per-game section lookup (`TotalA`, `TotalA/wine`, `TotalA/2..9` + `checkfile`) and `cfg_save`'s ini half are deleted |
| `cfg_load` read 69 keys | `cfg_load` (`config.c`) sets cnc-ddraw's own default for each field, the value it took for an absent key; a field not named is its zero default |
| `tagpu_cfg_defaults` owned four keys *unless the file carried them* (`typed()`) | it sets TA's values outright: `max_resolutions` 90 (a static assert bounds it at TA's 100-entry buffer), `toggle_borderless`, `lock_surfaces`, `singlecpu` off, `maintas`, borderless fullscreen; logs one `cfg:` line |
| three **held** flags greyed Display mode, Monitor and Frame cap when the file placed the window | gone; `vrow_held` greys only a lever's row (UI scale) or everything under `tagpu_defaults.off` |
| under `tagpu_defaults.off` the store had no say; the window came from `ddraw.ini` | `display`, `maxfps` and `window` are read under every launch (`tagpu_settings_placement`, `tagpu_settings_window`); nothing else can place a window |
| tacli wrote `center_window=0` so a tile survived the shell → game mode switch | a window the store placed gets `CENTER_WINDOW_NEVER` in code; one a mode switch would leave with a client corner on no monitor is moved the least distance that puts the window, decoration included, in the work area of the monitor it overlaps most (`dd_SetDisplayMode`) |
| the first-run migration stripped the window keys from `ddraw.ini` (`strip_ini`) | gone; the migration keeps its other steps |
| `renderer=` chose the backend (`gdi`, `vulkan`, `auto` = Vulkan) | Vulkan always, unless the harness lever **`tagpu_gdi.on`** is present at attach (`g_config.gdi`); GDI is otherwise only the lane Vulkan hands a failed session to |
| the compatibility warning pointed at `no_compat_warning` in `ddraw.ini` | the sentence is gone |
| cnc-ddraw's config tool (`cnc_ddraw_config_init`) got a `cfg_load` | it gets nothing: an early return, so the store is not attached in a foreign process |

**tacli.** `write_ddraw_ini` became `write_placement`: `display=window`,
`window=<tile x>,<tile y>,<W>,<H>` (`0,0` = the game's size; `--window` gives a `k ≠ 1` client)
and `maxfps`, **on every launch**, so a `display=fullscreen` clicked in an instance's menu lasts one
session. The store spells a cap only as `refresh`, `60`, `120` or `uncapped`, so `--maxfps` takes
0, 60, 120 or a negative value, and `--window` a client from 320×240 to 16384×16384 (the DLL drops
a `window=` line outside that whole, position included); `create` and `launch` refuse anything
else before creating anything. A launch deletes a `ddraw.ini` it finds in a gamedir, and the
mirror skips the template's. `--keep-dll` refuses a pinned build that still reads `ddraw.ini`
(its bytes carry the name): finding none, it would open borderless fullscreen, unplaced, and
write upstream's template beside itself. `place_window`'s clash
nudge now compares only windows on the instance's own X display: it used to move a window on a
private Xvfb away from another session's window on a different server.

**`tacli launch --shipped`** (decision 4) makes the gamedir what a player has and launches it:
it removes `tagpu_nowarp.on`, `tagpu_shield.on`, `tagpu_title.txt`, `tagpu_defaults.off` and
`totala.ini` (the next ordinary launch rewrites them from the instance's meta). The store's
`display`, `window`, `maxfps` and `resolution` are written by both sides — an ordinary launch
(`write_placement`, `ensure_store`) and a player's menu — so the store has an **owner**
(`meta["store"]`, `harness` or `player`): handing it over puts the current owner's four lines into
the meta and brings back the other side's from its last launch. So a `--shipped` session's menu
choices survive any number of ordinary launches in between, and the harness's size and tile
survive a `--shipped` one. A brand-new instance has its store deleted, so it gets a player's first
run. It refuses an arm file
(`tagpu_*.on/.off/.cfg`) with the exact `tacli arm … =off` to clear it, and refuses any flag that
writes (`--res`, `--window`, `--maxfps`, `--title`, `--sound`, `--unit-limit`, `--shield`,
`--defaults`). The registry is left as tacli keeps it (`UseXRandR=N` among it).

**The release.** `package.sh` copies no ini, `tagpu/release/ddraw.ini` is deleted, and
`README.txt` tells an upgrading player an old one is ignored and can be deleted.

## The upstream per-game hacks (decision 3)

`vhack`, `tshack`, `infantryhack`, `armadahack`, `stronghold_hack`, `mgs_hack`, `tlc_hack`,
`carma95_hack`, `sirtech_hack`, `flightsim98_hack` and `darkcolony_hack` were fields TA never set:
`[TotalA]` named none and each defaulted to `FALSE`. Each field is deleted and every reader folded
to its `FALSE` arm (`dd.c`, `ddsurface.c`, `winapi_hooks.c`, `utils.c`, `wndproc.c`,
`render_gdi.c`, `dllmain.c`). The two negated tests, `!tlc_hack` and `!infantryhack`, folded to
*true*: their conditions lost the term, not the branch.

What became unreachable went too, because nothing was left to reach it:

- **`vhack`'s upscale machinery** — `util_detect_low_res_screen`, `util_get_pixel`,
  `upscale_hack_width/height/active`, the per-title upscale sizes in `dd_SetCooperativeLevel`, the
  cursor clamp in `fake_GetCursorPos` and the mouse branch in both message paths — and the title
  flags only it read (`isredalert`, `iscnc1`, `isworms2`). **`iskkndx` stays**: `dds_Blt`'s KKND
  colour-fill case reads it, and that is not one of the eleven.
- **`sirtech_hack`'s mouse hook** — `mouse_hook_proc`, `g_mouse_hook`, `g_mouse_proc`.
- **The Learning Company block** in `fake_CreateWindowExA`, gated on `!game_section[0]`: TA always
  had a `[TotalA]` section, so the term was always false and the block never ran for TA.
- `window_state` / `upscaled_state`, written by the Alt+Enter toggle and the battle.net paths and
  read only by the deleted ini save.

Other upstream fields that are now compile-time constants (`devmode`, `boxing`,
`lock_mouse_top_left`, `no_compat_warning`, the hotkeys, …) stay as fields at their defaults.

## The fullscreen monitor (decision 5)

With no monitor chosen (`monitor=-`, or a stored one no longer attached), `util_default_monitor`
(`utils.c`) answers `util_target_monitor` and `util_target_refresh`:

- **windowed** — the window's own monitor; its frame placed it.
- **fullscreen with a windowed frame on record** (`window_rect`, from the store or from this
  session's window) — the monitor that frame is on. `util_toggle_fullscreen` records the window's
  client origin on the way out, so the frame is where the window *is*: on Windows a keyboard move
  or a snap updates `window_rect` nowhere else. The frame's monitor is the one its client overlaps
  most, so a window dragged partly past an edge, or with its origin in the gap between two monitors
  of different sizes, still counts; a frame on no monitor (its screen unplugged) counts as none. This is a refinement of the decision's
  "primary": without it, Alt+Enter from a window on a side monitor would go fullscreen on the
  primary.
- **fullscreen with no frame** — the **primary**, the monitor at the desktop's origin. Before,
  this was the window's own monitor, i.e. wherever wine created the window: on the reference
  setup a first run with no store went fullscreen on the portrait `HDMI-0` and stored
  `monitor=\\.\DISPLAY2` (MEASURED 2026-09-24, before this landing).

`g_config.fullscreen` and `window_rect` are read on the render thread too (`fpsl_init`); both are
aligned values, and every answer a racing read produces is a monitor that exists: a point on
none falls through to the primary.

## The decisions (the owner, 2026-09-24)

1. Rows in test instances are **live**; tacli rewrites its three keys at every launch.
2. The GDI lane's switch is the lever file **`tagpu_gdi.on`**, not a store key.
3. The strip goes **deep**: the file machinery, the template and the per-game hacks.
4. **`--shipped`** rides with this landing.
5. The first-run monitor is the **primary**.

## Verified by running it (2026-09-24)

Private Xvfb displays (`:93` 1920×1200, `:95` 3840×1200) and a nested Xephyr (`:96`, two
Xinerama heads, 1280×1024 at 0,0 = wine's primary and 1920×1080 at 1280,0) inside `:95`; the RTX
4070 binds on the Xvfb displays. Every log line quoted is the DLL's own.

1. **A player's install** — a gamedir with the DLL and the weights only, launched by hand
   (`WINEPREFIX=… WINEDLLOVERRIDES=ddraw=n,b wine TotalA.exe`): no `ddraw.ini` appeared; the first
   run migrated and wrote the defaults; `cfg: max_resolutions 90, toggle_borderless, lock_surfaces,
   singlecpu off, maintas; display=fullscreen …; renderer Vulkan`. Options → Visuals: Display mode
   and Frame cap live; Monitor, GPU and Shadow quality greyed for their own reasons (one screen,
   a first run's empty GPU list, the Classic++ preset). Frame cap clicked to 60 → `maxfps=60` in
   the store → kept across a relaunch.
2. **An old install** — the same gamedir plus a `ddraw.ini` with `renderer=gdi`, `windowed=true`,
   `fullscreen=false`, `maxfps=120`, `posX/posY`, `width/height`, `max_resolutions=0`,
   `singlecpu=true`, `lock_surfaces=false`: Vulkan, borderless fullscreen 1920×1200, the store's
   cap, no window placed; the file byte-identical afterwards; Display mode and Frame cap live.
3. **tacli** — a bare control launch (`settings: impure.cfg IGNORED but for display, maxfps and
   window …`, `window=placed (center_window never)`); `scenario load cob-tank` with its
   `setup.res`; `--res 800x600 --window 1200x900 --maxfps 0` (a 1200×900 client, `maxfps=0`,
   `maxfps=uncapped` in the store); `--maxfps 30` refused before anything was written. On the
   3840-wide display, **a tile at 1064,0 stayed there through the 640×480 → 1024×768 switch**
   (cnc-ddraw's `auto` would re-centre it at 1408). Display mode clicked to fullscreen in a
   `--defaults` instance's menu → a 3840×1200 borderless window and `display=fullscreen` in the
   store → stop → a bare relaunch opened at the tile with `display=window` rewritten.
4. **The GDI lane** — `tacli arm <i> gdi.on`: `renderer GDI (tagpu_gdi.on)`, no `vk: swapchain`
   line, the shell and a skirmish drawn in the window capture.
5. **The package** — `package.sh` → `ddraw.dll`, `full.w32.bin`, `tiny.w32.bin`, `README.txt`.
6. **The hacks pruned** — a skirmish, then Tab → EXIT → MAINMENU → yes → EXIT, on Vulkan and on
   GDI: the process ended and no `ErrorLog.txt` was written.
7. **`--shipped`** — a fresh instance's gamedir afterwards held exactly what gate 1's hand launch
   left (`ddraw.dll`, `impure.cfg`, `impure-migration.txt`, `impure-patch.ufo`, `tagpu_vk.gpus`),
   the game borderless fullscreen by itself; refused with `--res` and with `tagpu_zoom.on`; an
   ordinary launch afterwards rewrote every harness file and the placement; a `maxfps=120` in the
   store survived a second `--shipped` launch.
8. **The fullscreen monitor**, on the nested two-head server with the GDI lever: no store → the
   target logged `0,0-1280,1024` and the window came up 1280×1024 at 0,0 (the primary); a stored
   frame at 1400,100 with `display=fullscreen` and a detached `monitor=\\.\DISPLAY9` → target
   `1280,0-3200,1080`, window 1920×1080 at 1280,0.
9. **The review's fixes** (two `high` reviewers, then a re-review of the fixes). Store ownership,
   on a fresh instance: `--shipped`, menu keys set to `maxfps=120 display=window
   resolution=800x600` → an ordinary launch wrote `resolution=1024x768`, the tile and `maxfps=60`,
   with the player's three kept in the meta → a second `--shipped` launch came up `display=window
   maxfps=120 window=default` with the player's lines back and no harness file → an ordinary launch
   restored the harness lines. On a `--defaults` instance, two `--shipped` launches in a row left
   the harness's `res` and tile at 1024×768 with the player's `resolution=800x600` in the store,
   and a control launch after them restored `resolution=1024x768`. `create --maxfps 30` and
   `create --window 200x100` refused with no instance directory left; `--keep-dll` over `main`'s
   build refused and wrote no ini. On the nested two-head server (no WM), a stored frame at
   2800,700 came up with its 640×480 client at **2556,596** (the 4-pixel border ends on head 1's
   edge), one at -300,-200 at **4,30** (the title bar at 0,0), one at 1000,100 straddling both
   heads stayed, one at 2500,500 stayed; from 2500,500, Display mode → Fullscreen went 1920×1080
   at 1280,0 and back → Window returned to 2500,500.

## Gaps

- **The first-run fault itself was not reproduced.** It needs a window manager that creates the
  window on a side monitor. Wine creates it at the origin, and xfwm4 in the nested server did not
  move it, so `main`'s DLL came up on the primary too and the gate has no before/after. The rule
  is verified; the reference setup's failure is not re-run, because that means a fullscreen game
  on the owner's display.
- **xfwm4 keeps a fullscreen window on the monitor it was on.** With xfwm4 running, the frame case
  targeted head 1 correctly and the window still ended on head 0; without a WM it landed on head 1.
  Wine asks the WM to go fullscreen, and the WM picks the monitor. The reference setup's WM
  (mutter) is not known to behave this way, and it was not tested.
- **The keyboard-move case is Windows-only.** Under wine `WM_MOVE` keeps `window_rect` current on
  every move, so recording the origin at the toggle cannot change anything there; on Windows it is
  argued from the code (`wndproc.c` updates the frame only inside a drag), not run.
- **A `--keep-dll` A/B across this landing is refused, not served**: a pre-removal build runs with
  a tree from before it, whose tacli still writes the ini.
- `cnc-ddraw.vcxproj` still lists `src\ini.c`; that project file already listed none of the
  `tagpu_*` sources and does not build this DLL.
- The `renderer=` spellings in dated records across the notes are the ini key those launches used.

Related: [the settings store](renderers.html#210b-the-settings-store-every-value-on-the-visual-screens-is-ours-decided-2026-09-23),
[the fork's settings](gpu-status.html) §2.8b, [windowed mode](windowed-mode.html),
[the DirectDraw boundary](api-wrappers.html), [tacli](tacli-design.html).
