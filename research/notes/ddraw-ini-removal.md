# Removing ddraw.ini — the handoff

**Status: planned, not started.** The owner asked for it on 2026-09-24, as its own landing. This
page is the brief for whoever builds it: what reads and writes `ddraw.ini` today, what each
value is for, the plan, the traps found while surveying, the gates, and the decisions that are
the owner's. Everything under *Today* was read in the source at `b454cbb`; check it again
before building on it.

**The goal.** `impure.cfg` becomes the one settings file. A player's game has no `ddraw.ini`,
and nothing written anywhere else can hold a row of the Visuals menu. Today a key in `ddraw.ini`
wins over the menu and greys its row. tacli writes those keys into every instance, so an
owner-facing tacli launch cannot test the menu: Display mode, Monitor and Frame cap are greyed.
That is how this came up.

**Why the file still exists.** Inheritance, not need. The fork is cnc-ddraw, whose whole
configuration is this file. Of the ten values the release ships, three change anything a player
gets (below). The seven override keys serve tacli. Nothing else in the DLL reads the file.

## Today

### What the DLL does with it

In the order `cfg_load` runs (`config.c`):

1. **`cfg_init` finds the file**: `CNC_DDRAW_CONFIG_FILE` if set, else `ddraw.ini` beside the DLL.
   **If it is missing, `cfg_create_ini` writes upstream's template**: a string of about 1 700
   lines (`config.c`, `cfg_create_ini`), with a `[TotalA]` section among hundreds of other games'.
   So not shipping the file does not get rid of it; the DLL makes one on first run.
2. **`tagpu_settings_attach` runs before the parse** (`tagpu_settings.c`). With no `impure.cfg` it
   runs the first-run migration, whose `strip_ini` deletes `maxfps`, `windowed`, `fullscreen`,
   `posX`, `posY`, `width` and `height` from `[ddraw]` and `[TotalA]` (backup
   `ddraw.ini.migrated`).
3. **The parse.** `cfg_get_game_section` picks a per-game section by process name (`TotalA`,
   `TotalA/wine` under wine, `TotalA/2..9` with a `checkfile`). Each key is looked up there first,
   then in `[ddraw]`. `cfg_load` reads 69 keys. 57 of them land in a `g_config` field that
   something outside `config.c` reads, upstream's per-game hacks among them, at their defaults
   (`infantryhack`, `tlc_hack`, `carma95_hack`…). The other twelve fill the window rectangle, the
   hotkeys and `game_handles_close`.
4. **`tagpu_cfg_defaults`** (`tagpu_cfg.c`) takes ownership of four keys, but only when the file
   does not carry them (`typed()`): `max_resolutions` 90 (TA's mode buffer holds 100 and its
   callback `0x4B5330` does not bounds-check), `toggle_borderless` on, and `windowed` +
   `fullscreen` as one pair (borderless fullscreen). It then latches three **held** flags:
   `s_displayHeld` (`windowed` or `fullscreen` present), `s_maxfpsHeld` (`maxfps`),
   `s_windowHeld` (`posX`/`posY`/`width`/`height`). A held key beats the store. Unheld, the store's
   `display`, `maxfps` and `window` are applied. It also forces `save_settings = 0`.
5. **`tagpu_menu.c`'s `vrow_held`** greys the row a held flag names: Display mode, Monitor (display
   *or* window held) and Frame cap.
6. **`cfg_save`** at exit puts a windowed frame into the store unless the window is held. Its
   upstream half, which writes the window keys back into the file, never runs, because
   `save_settings` is 0.

Two more mentions: `dd.c`'s renderer switch reads `renderer` (`auto` and `vulkan` give Vulkan,
`gdi` gives GDI, anything else is logged and gives Vulkan), and `dllmain.c`'s compatibility-mode
warning tells the player to set `no_compat_warning` in `ddraw.ini`.

### What the shipped file changes

`tagpu/release/ddraw.ini` against the DLL's behaviour with no file:

| key | shipped | with no file | matters |
|---|---|---|---|
| `lock_surfaces` | true | false | **yes**: `dds_Lock` takes the surface's own critical section only when it is set (`ddsurface.c`; `render_gdi.c` reads it too) |
| `singlecpu` | false | **true** | **yes**: true pins the process, render thread included, to one CPU (`dllmain.c`, `dd.c`) |
| `maintas` | true | false | **yes**: the aspect-preserving fit (`dd.c`, `utils.c`, `wndproc.c`) |
| `renderer` | vulkan | auto = Vulkan | no |
| `savesettings` | 0 | 1, then forced to 0 by `tagpu_cfg_defaults` | no |
| `border`, `vsync`, `adjmouse`, `maxgameticks`, `minfps` | true, false, true, 0, 0 | the same | no |

Upstream's template carries `lock_surfaces=true` and `singlecpu=false` in its `[TotalA]`
section, and `maintas=false` in `[ddraw]`, so a player with no file gets the first two from the
template and still misses `maintas`.

### What tacli does with it

- **`write_ddraw_ini`** (`tools/tacli`) writes the whole file: `renderer=vulkan`,
  `windowed=true`, `fullscreen=false`, `maintas`, `vsync=false`, `maxfps`, `adjmouse`,
  `border=true`, `savesettings=0`, **`center_window=0`**, `posX`/`posY` (the tile),
  `width`/`height` when `--window` asks for a client unlike the game's size (k ≠ 1), and in
  `[TotalA]` **`max_resolutions=32`**, `lock_surfaces`, `singlecpu=false`, `maxgameticks=0`,
  `minfps=0`.
- **It runs only on some launches**: an explicit `--maxfps`, `--res`, `--window`, a size picked in
  the menu of a `--defaults` instance, or a tile found off-screen. A bare launch writes nothing, so
  a hand edit survives it. `scenario load` rewrites the file whenever the fixture has `setup.res`.
- `ddraw.ini` is in `PRIVATE_FILES` (a real copy per gamedir, never a symlink).
- `ensure_store` already writes into `impure.cfg`: an empty store when the gamedir has none, and
  `resolution=WxH` from `--res`.
- **The GDI lane has no other switch**: a GDI measurement hand-edits `renderer=gdi` into the
  gamedir's `ddraw.ini` (ta-drive `references/measuring.md`).

### The release

`tagpu/release/package.sh` copies `tagpu/release/ddraw.ini` into the package, and
`tagpu/release/README.txt` lists it ("keep renderer=vulkan") and describes the first-run
migration. CI (`.github/workflows/build.yml`) runs `package.sh`.

## The plan

### 1. The DLL reads no ddraw.ini

- **`cfg_init` opens no file** and `cfg_create_ini` goes, the template with it; so do
  `CNC_DDRAW_CONFIG_FILE`, the game-section lookup and `cfg_save`'s ini half. The upstream code
  deleted here goes the way the GL strip took it: our feature, our deletion, no stub left behind.
- **The values TA needs are set in code**, in `tagpu_cfg_defaults`, which stops asking `typed()`:
  `lock_surfaces` on, `singlecpu` off, `maintas` on, `max_resolutions` 90, `toggle_borderless`
  on, `save_settings` 0, and the display pair from the store. Every other key keeps its compiled
  default, which is what a player gets today.
- **The held flags and `vrow_held`'s three cases go.** No row can be held by a file any more.
  The lever files (`tagpu_classicpp.on/.off`, `tagpu_ss.off`, `tagpu_fps.on`, `tagpu_hud.on/.off`,
  `tagpu_classicpp.cfg`) are not this landing.
- **`strip_ini` goes.** A file the DLL never reads needs no cleaning. The migration keeps its
  other steps (moving an earlier menu's lever files aside).
- **`ini.c`** has no reader left once `config.c` and `tagpu_cfg.c` stop parsing (checked: no other
  file calls `ini_create` or `ini_get_*`). Delete it.
- **The compatibility warning** stops naming `ddraw.ini`.
- **Whether to prune the upstream hacks** (`infantryhack` and the rest) is a separate question.
  Their fields stay at their defaults either way; leaving them is the smaller diff.

### 2. The store holds tacli's pins

- **Under `tagpu_defaults.off` the store's `display`, `maxfps` and `window` still apply.** Today
  a control launch (any tacli launch without `--defaults`) ignores the store, and its window is
  placed by `ddraw.ini`. Without the file, those three keys are the only place it can come from.
  Every other key stays ignored under `.off`, `resolution` included (the registry's size stands,
  as today).
- **`center_window` must be *never* whenever the store places the window.** At cnc-ddraw's
  default (auto), `dd.c`'s "temporary fix" block re-centres the window when TA switches to a mode
  larger than the window's recorded size, as when the 640x480 shell gives way to a bigger game.
  tacli writes `center_window=0` today for exactly this reason. Without it, a tiled window jumps.

### 3. tacli writes the store instead

- **`write_ddraw_ini` becomes a store writer**: `display=window`, `window=x,y,W,H` (the tile; `W,H`
  the `--window` client or `0,0`, cnc-ddraw's "the game's size", which `frame_ok` accepts), and
  `maxfps`. The store spells `maxfps` only as `refresh`, `60`, `120` or `uncapped` (`O_MAXFPS`), so
  `--maxfps 0` → `uncapped`, a negative → `refresh`, 60 and 120 as they are, anything else refused
  with a message.
- **It writes them on every launch**, not only on the flags that trigger it today. The store is a
  file the menu writes: a `display=fullscreen` clicked in a test instance, kept by a bare
  relaunch, opens that instance fullscreen on the owner's 4K screen.
- **`ddraw.ini` leaves `PRIVATE_FILES`**, and a launch deletes one it finds in a gamedir, so no
  stale file suggests it still counts.
- **The GDI lane gets a switch of its own** (decision 2), and `measuring.md` its recipe.

### 4. The release

`package.sh` stops copying it, `tagpu/release/ddraw.ini` is deleted, and `README.txt` loses the
line and gains one for players upgrading: an old `ddraw.ini` is ignored and can be deleted.

## Traps found while surveying

- **The template.** A missing file is re-created from upstream's template, so a test that deletes
  `ddraw.ini` and sees the game run proves nothing until `cfg_create_ini` is gone.
- **tacli instances run a different mode list from players**: tacli writes
  `max_resolutions=32` into `[TotalA]`, a player gets the DLL's 90. The removal ends that.
- **A control launch is store-blind today** (`tagpu_settings_get` returns nothing under
  `tagpu_defaults.off`). Plan step 2 is what keeps those launches on their tile. Test it
  explicitly.
- **The first run of a player-like launch picks the monitor wine created the window on.**
  `monitor=-` means the window's own monitor. On the reference setup, a hand launch with no store
  went borderless fullscreen on the portrait `HDMI-0` (1080x1920 at 0,112) and stored
  `monitor=\\.\DISPLAY2`. MEASURED 2026-09-24. Wine does report the 4K panel as the primary
  display [INFERRED: the desktop mode `inject_resolution` takes is 3840x2160]. Not this landing's defect, but its gates will
  meet it; defaulting to the primary monitor is decision 5.
- **`tagpu_cfg.c`'s header reasons from the shipped file** ("the presence test means what it says
  only because we do not ship these keys"). It is rewritten with the module, not annotated.

## Gates

**Verified by running it**, per `CLAUDE.md`:

1. **A player's install**: a gamedir with the DLL and nothing else (no `ddraw.ini`, no
   `impure.cfg`), launched by hand (`WINEPREFIX=<prefix> DISPLAY=<d> WINEDLLOVERRIDES=ddraw=n,b
   wine TotalA.exe`). No `ddraw.ini` appears. The log shows the TA values (`lock_surfaces`,
   `singlecpu`, `maintas`). Every Visuals row is live, and a change survives a relaunch in
   `impure.cfg`.
2. **An old install**: the same with a v0.2-era `ddraw.ini` carrying `windowed`, `maxfps=60` and
   `posX`. Nothing in it applies and no row is greyed.
3. **tacli, every path**: a bare launch, `--res`, `--window` (k ≠ 1), `--maxfps 0` (read
   `frame cap:` in the log), a control launch without `--defaults`, and `scenario load` of a
   fixture with `setup.res`. Each opens on its tile, off the owner's 4K screen (read the
   launch line's `at x,y`, and stop at once if x < 4920, the ta-drive skill's tile rule). Set
   Display mode to fullscreen in the menu of a test instance on a **private Xvfb**, then relaunch
   bare: the tile comes back.
4. **The GDI lane** through its new switch.
5. **The package**: `package.sh` output has no `ddraw.ini`.

**Review at high**: the landing touches `tagpu/ddraw/**` and `tools/tacli`, and writes user state
(the store). **The docs pass**: this page moves from plan to record; renderers.md §2.10b
(14 mentions of the file), gpu-status (4), roadmap (3), api-wrappers (3), gui-renderer (2),
gpu-posing (2), windowed-mode, field-notes and binary-patches; the ta-drive skill (`SKILL.md`'s
"a bare launch writes no ddraw.ini", `references/modules.md` on held rows and the migration,
`references/measuring.md` on the GDI lane and `grep maxfps`, `references/levers.md` on the
renderer); `tagpu/release/README.txt`. The mentions in deep-ta-zero and candidates-community are
about other projects' files and stay.

## Decisions for the owner

1. **Rows in test instances.** Recommended: live. tacli rewrites its three keys at every launch,
   so a menu change lasts one session. The alternative is a harness marker that greys them again:
   a second mechanism, for a view only agents see.
2. **The GDI lane's switch.** Recommended: a lever file, `tagpu_gdi.on`, read at attach and
   logged, like every other harness lever. The alternatives are a store key (then the menu could
   show it, which a player has no use for), or dropping forced-GDI measurements and keeping GDI
   only as the fallback when Vulkan fails.
3. **How deep the strip goes.** Recommended: the file machinery and the template go (plan step
   1); the upstream hack fields stay, at their defaults, for a later pass.
4. **An as-shipped tacli launch for hand-overs.** The owner's own test on 2026-09-24 needed a hand
   launch, because tacli also writes `tagpu_nowarp.on`, a `totala.ini` and the title file. A flag
   that writes none of it (say `--shipped`) would make that a tacli command. Small, and related,
   but not required by the removal.
5. **The first-run monitor.** Default `monitor=-` to the primary monitor rather than the one wine
   first created the window on. A separate fix, raised here because the gates will meet it.

Related: [the settings store](renderers.html#210b-the-settings-store-every-value-on-the-visual-screens-is-ours-decided-2026-09-23),
[windowed mode](windowed-mode.html), [the DirectDraw boundary](api-wrappers.html),
[tacli](tacli-design.html).
