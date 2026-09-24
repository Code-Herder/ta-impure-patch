# Modules with their own workflow

Extra weapons, the COB trace, multiplayer, and the render-options screen with its GPU row. Each
is driven through `tacli` like everything else; what differs is the setup around it.

1. [Extra weapons](#extra-weapons)
2. [The COB script trace](#the-cob-script-trace)
3. [Multiplayer: two instances in one game](#multiplayer-two-instances-in-one-game)
4. [The render-options screen and the GPU row](#the-render-options-screen-and-the-gpu-row)

## Extra weapons

`tagpu_weapons.c` lifts "three weapons per unit" to `Weapon4..N` (capacity 16). It is a play
default under `--defaults`; on a `tacli` instance it is **off unless armed before launch**, and
stock units run the untouched engine code either way (`research/notes/extra-weapons.md`).

```bash
tools/tacli launch w1; tools/tacli stop w1          # create the instance dir
tools/tacli arm w1 weapons.on                        # must exist at DLL attach
tools/extra_weapons_fixture.py                       # builds scenarios/content/wpn-test.ufo (gitignored)
ln -s $PWD/scenarios/content/wpn-test.ufo tagpu/instances/w1/gamedir/   # the test units
rm -f tagpu/instances/w1/catalogue.json              # cached type list is now stale
tools/tacli scenario load w1 wpn-llt10 --restart     # two ten-laser towers vs solars
tools/tacli weapons w1                               # every slot of every unit + counters
tools/tacli log w1 -g "weapons: (loader|VIOL|MISM)"
```

- **`tacli weapons <inst> [idx…]` is the oracle**: per slot the state byte, weapon, target,
  reload, heading, pitch, stock, aim result and COB thread, plus `armed`, the C-path hit counters
  and projectile launches per slot. It works **unarmed** too — that instance is your control. With
  stock content and the module armed, every counter but `loader`/`stock_splice` must read 0 and
  `mismatch`/`violation` must be 0; that is the regression check.
- It also prints each unit type's `CRC_weapons` and `CRC_all`, the unit-sync values. Armed and
  unarmed differ for exactly the types carrying `Weapon4+`. In a multiplayer game TA **disables**
  a type the peers disagree about (it vanishes from `tacli units` on both sides) and starts
  anyway, silently.
- **Content goes in a `.ufo`, never as loose files** (the engine finds a loose `units/*.fbi` and
  then drops the type). `tools/hpipack.py` writes and reads the archive, `tools/cobclone.py`
  gives a COB per-weapon script copies, `tools/extra_weapons_fixture.py` rebuilds the shipped test
  pack from the game. After adding or removing archives, delete the instance's `catalogue.json` or
  `scenario load` refuses the new type at validation.
- Fixtures: `wpn-llt10`, `wpn-peewee4`, `wpn-badtgt`.

## The COB script trace

`tagpu_cobtrace.on` at DLL attach makes the DLL log every COB thread the engine starts, refuses,
returns, kills or draws a random number for — one tab-separated line each, stamped with the sim
tick — to `gamedir/log/tagpu_cobtrace.log`, the log sink's second stream (it rotates like
`tagpu.log`; `tools/talog.py run <gamedir> --stream tagpu_cobtrace` joins a run's parts). The value is a unit-type filter. Line contract:
`research/notes/tacob-design.md` §"The trace contract"; the engine seam:
`exe-reverse-engineering.md` §"The COB engine".

```bash
tools/tacli arm c1 cobtrace.on=ARMPW native.on=all   # native.on because the pose oracle lives in that pass
tools/tacli scenario load c1 cob-kbot --restart
sleep 8; tools/tacli arm c1 posedump.on              # one pose dump, header `posedump: tick= idx=`
tools/tacli log c1 -g 'cobtrace:'                              # ARMED … filter=,ARMPW,
tools/talog.py run tagpu/instances/c1/gamedir --stream tagpu_cobtrace | cut -f1-8 | head
```

- **`tools/cobtrace_fixtures.py`** runs the nine class scenarios (`scenarios/cob-*.json`) this way
  and keeps `cobtrace.log`, `posedump.txt` and tacli's `apply.json` per class under
  `research/notes/evidence/cobtrace/`. It parks the camera on the traced unit (`tacli eye`) before
  dropping `posedump.on`: the pose oracle dumps the first unit the native pass draws, and an
  aircraft or a ship has left the spawn view by the time it has done anything.
- **Sight radius before weapon range.** A unit 250 wu from an enemy it cannot see never aims. Put
  the target inside the shooter's `SightDistance`, not just its range.
- **A Hawk is air-to-air**; ordered at a ground unit it flies over it and does nothing.
- The file is truncated at every launch and flushed per line, so `tacli stop` loses nothing.
- `tools/tacob pose-check` reads the pose dump; the dump's fields are what it exists for.

## Multiplayer: two instances in one game

Works over loopback on the stock wine these instances use, with Microsoft's own DirectPlay in
front of wine's builtin (which implements the client half only and cannot create a session).
`tools/dpinstall.sh` installs it into a prefix, `tools/dptest/` proves a prefix can host before you
go blaming the game, and `research/notes/networking-lobbies.md` has the protocol.

```bash
tools/tacli launch h1 --dplay --free-dplay-port     # the host
tools/tacli launch j1 --dplay                       # the joiner
tools/mp_lobby.sh h1 j1 'Two Continents'            # menus -> battle room -> live
MP_NO_START=1 tools/mp_lobby.sh h1 j1               # stop in the battle room
```

- `--dplay` installs native DirectPlay into that instance's prefix and appends the overrides to
  `ddraw=n,b`. Sticky per instance; a single-player instance keeps wine's builtin.
- `--free-dplay-port` kills a stale `dplaysvr.exe`. DirectPlay's name server outlives the game that
  started it and owns UDP 47624 **machine-wide**, so a leftover one makes the next host fail
  `Open(DPOPEN_CREATE) = DPERR_GENERIC`, which looks exactly like a broken prefix. Put it on the
  **hosting** launch only: doing it while a peer is hosting takes that game down too. Never
  `pkill -x dplaysvr.exe` by hand while another agent's game is hosting.
- After running `dptest` against a prefix, **let it settle** before launching TA there: a wineserver
  still shutting down produced a launch with no process and no `ErrorLog.txt`.
- **`START` ungreys only when every player is ready, the host included.** Each client lists
  *itself* as row 0, so the host's own toggle is `READY0` on its screen and the joiner's is `READY0`
  on theirs. `PLAYER0`/`READY0`/`PLAYER1`… are created at runtime and sit past the end of the
  default `ui` snapshot; reach them with `ui <inst> show READY1`.
- **Lobby state syncs**: set the map on the host and read it back on the joiner
  (`ui <join> show MAPNAME`) as a cheap proof the link is live.
- **Every peer creates its own units.** `scenario apply` on a peer replicates the units that peer
  owns to the others, through TA's own create packet. `owner` is the LOCAL player index — each
  peer is owner 0 on its own screen — so units a file gives owner 1 are created for the other
  player's slot on this peer alone, and no peer simulates them. For a fight between peers, apply
  one half on each: `scenario apply h1 limits-mp-west` and `scenario apply j1 limits-mp-east`,
  both written as owner 0. Leave `clear_existing` false, or a peer kills the other's commander
  locally.
- **To compare peers, pause and read both rosters.** `tacli keys h1 shift pause` sends the game's
  own `Pause` (the leading token is the one a `keys` call drops), which pauses every peer; confirm
  with `tacli peek <i> '*511DE8+38A51:1'` reading 1 on each. Then `tacli roster <i> --json` on
  both: match units by `engine_index`, which is the same slot on every peer, with `owner` flipped
  (0 ↔ 1 for two players). Stationary units agree exactly. Tab opens the options panel and does
  not pause a network game.
- **Each peer draws its own units in player 0's colour** on the Vulkan lane; the engine's own frame
  is right. A renderer limit (`gpu-status.md` §3.2), not a network fault.
- `SELPROV`'s `SELECT` crashes the game on the non-TCP/IP rows; select *Internet TCP/IP
  Connection For DirectPlay* **by name, never by row number** — it is row 0 under wine's builtin
  DirectPlay and row 3 under the native one.

## The render-options screen and the GPU row

`tagpu_menu.c` adds our rows to **Options → Visuals**: the frame-rate readout (`VFPS`), the GPU
(`VGPU`), the video mode and monitor (`VMODE`, `VMON`), the UI scale (`VSCALE`) and the visual
switches. **Every row is kept in `impure.cfg`**, the settings store (`renderers.md` §2.10b): a
click is in force within half a second and survives a relaunch, and the menu writes no lever
file. `tagpu_menu.off` disables the screen. Its rows are ordinary gadgets:

```bash
tools/tacli ui <i> show VGPU                  # stages, stage, grayed
tools/tacli ui <i> click VGPU                 # cycle; the lane rebuilds within a frame
cat <gamedir>/tagpu_vk.gpus                   # "<0|1> <name>" per device, 1 = discrete
cat <gamedir>/impure.cfg                      # every row; gpu=<name>, stored BY NAME
tacli log <i> -g '^settings:'                 # what the store loaded, migrated or refused
```

- **A lever holds its row, greyed.** `tagpu_classicpp.on/.off`, a menu key inside
  `tagpu_classicpp.cfg`, `tagpu_ss.off`, `tagpu_fps.on`, `tagpu_hud.on/.off`, and `ddraw.ini`'s
  `fullscreen`/`windowed`/`maxfps`/`posX`/`posY`/`width`/`height` each beat the store for what they
  name. tacli's own `ddraw.ini` carries the window keys, so **Display mode, Monitor and Frame cap are
  greyed in every instance** — the tile is the lever. `tacli arm` still drives the others for A/Bs.
- **Under `tagpu_defaults.off` (every launch without `--defaults`) the store has no say and every
  row but Shadows is greyed.** Shadows is then the engine's own shadow switch. Test the menu with
  `--defaults`. The engine's Gamma, size and option word come from the one shared `user.reg`, which
  a `--defaults` instance saves its store's values into at every game entry — peek them before
  treating a control launch as stock.
- **The store also owns the engine's Gamma, screen size and shadow bits** under `--defaults`: the
  registry is still loaded and saved, but memory is the store's after the startup load, and a
  later reload (a `scenario load`) keeps memory's (`tacli log <i> -g 'registry reload'`). tacli
  writes the instance's `--res` into the store as `resolution=WxH`, or `native` would win. The
  Visuals screen has no `SHADING`, `ANTI` or `BSHADOWS` gadgets; the in-game `VISUALRT` carries
  `GAMMA`, `RESTORE` and `UNDO` only.
- **tacli creates an empty `impure.cfg` before every launch.** A missing store is the DLL's
  first-run signal: it renames the files an older menu wrote to `*.migrated`, strips `ddraw.ini`,
  and leaves `impure-migration.txt`, after which a missing store only gets the defaults. To test the
  migration, delete the store and the record, then launch the exe by hand in the gamedir
  (`WINEPREFIX=<prefix> DISPLAY=<d> WINEDLLOVERRIDES=ddraw=n,b wine TotalA.exe`) on a private
  Xvfb — it goes fullscreen. A migrated instance has lost its tile keys from `ddraw.ini`.

- **The GPU list is one launch behind.** The captions live in a generated `.GUI` written at DLL
  attach, and a Vulkan instance cannot be created there, so a worker enumerates after the render
  thread is up and writes `tagpu_vk.gpus` for the *next* launch. On a gamedir that has never run
  this DLL the row reads `(not listed yet)` and is greyed; relaunch once.
- **The row is greyed unless there are at least two devices.** It binds the Vulkan device only.
- **Its first stage is Auto** (`gpu=auto`): the lane ranks discrete > integrated > virtual > CPU,
  then device-local memory, and logs `vk: Auto: <name> (type rank N of 4, M MB device-local)`. A
  named choice plates the device actually bound; a stored name no longer present binds the Auto
  pick and logs `the requested GPU "…" is not among the devices present - Auto instead`.
- **llvmpipe is not offered while a GPU is present**, and a stored choice of it is refused
  (`vk: the requested GPU "…" is a software rasteriser`), so on the reference setup the row is
  Auto alone and greyed. With llvmpipe bound the game dies on opening Options → Visuals and at a
  3840x2160 start-up; that only happens now on a machine with no GPU.
- **The Frame cap row's first stage is Refresh** (`maxfps=refresh`, the target monitor's rate,
  logged as `frame cap: Refresh = N fps`); tacli's `ddraw.ini` `maxfps` holds the row.
- **At most eight devices are listed** (our cap; a stage button's art index is clamped by the
  engine, so a row past four stages draws the four-bar plate and still works). Names are truncated
  to 31 characters at a word boundary.
- **A dialog over the world at zoom ≠ 1 takes 1:1 clicks** (the transform reads the engine's
  ownership bit at `main+0x37EBE`), so the rows are driven at any zoom; `SHARE.GUI` is the one
  screen whose zoomed clicks are a known gap.
