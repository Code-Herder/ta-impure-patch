# Modules with their own workflow

Extra weapons, the COB trace, multiplayer, many unit types, weapons past 256, the new data keys,
the render-options screen with its GPU row, and a remote Windows machine. Each is driven through `tacli`; what
differs is the setup around it.

1. [Extra weapons](#extra-weapons)
2. [The COB script trace](#the-cob-script-trace)
3. [Multiplayer: two to ten instances in one game](#multiplayer-two-to-ten-instances-in-one-game)
4. [Many unit types: the synthetic mod](#many-unit-types-the-synthetic-mod)
5. [Weapons past 256: the test weapons](#weapons-past-256-the-test-weapons)
6. [The new data keys: the key fixtures](#the-new-data-keys-the-key-fixtures)
7. [The render-options screen and the GPU row](#the-render-options-screen-and-the-gpu-row)
8. [A remote Windows machine](#a-remote-windows-machine)

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
tools/tacli log w1 -g "weapons: (unit-data|loader|VIOL|MISM)"   # each load empties, then fills
```

- **`tacli weapons <inst> [idx…]` is the oracle**: per slot the state byte, weapon, target,
  reload, heading, pitch, stock, aim result and COB thread, plus `armed`, the C-path hit counters,
  projectile launches per slot, and each extended type's cached `AimFromWeaponN,QueryWeaponN`
  pieces from slot 3 on (`-1` = not asked yet). It works **unarmed** too — that instance is your control. With
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

## Multiplayer: two to ten instances in one game

Works over loopback on the stock wine these instances use, with Microsoft's own DirectPlay in
front of wine's builtin (which implements the client half only and cannot create a session).
`tools/dpinstall.sh` installs it into a prefix, `tools/dptest/` proves a prefix can host before you
go blaming the game, and `research/notes/networking-lobbies.md` has the protocol.

```bash
tools/tacli launch h1 --dplay --free-dplay-port     # the host
tools/tacli launch j1 --dplay                       # a joiner, as many as nine
tools/mp_lobby.sh --map 'Two Continents' h1 j1      # menus -> battle room -> live
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
  locally. A peer's cap counts its commander: at 1500 a player a fixture brings 1499, and 1500 is
  refused whole. Ten peers, one fixture each: `scenarios/limits-tier2-p0` … `p9`.
- **A dead commander wipes its peer's army on every peer** while the registry's
  `MultiCommanderDeath` is 1, the lobby's default (the engine's gate is `ActiveCommanderDeath`,
  `main+0x37EF6`). In a fight that looks exactly like a network drop: one owner's units vanish from
  every roster at once. For a stress fight, `wine reg add` it to 0 in your own prefix before the
  host launches, read `*511DE8+37EF6:1` as 0 on every peer, and set it back after (the registry is
  one file for every instance).
- **To compare peers, pause and read both rosters.** `tacli keys h1 shift pause` sends the game's
  own `Pause` (the leading token is the one a `keys` call drops), which pauses every peer; confirm
  with `tacli peek <i> '*511DE8+38A51:1'` reading 1 on each. Then `tacli roster <i> --json` on
  every peer (`tacli units` lists unit TYPES, not live units): match units by `engine_index`,
  which is the same slot on every peer, and remember `owner` is each peer's own seat numbering.
  Stationary units agree exactly. A moving unit on a remote peer is where its owner last reported
  it, so compare against the owning peer (the one where the unit is owner 0) and scale the
  tolerance by the unit's speed; the pause itself lands a few ticks apart (`GameTime`,
  `*511DE8+38A47:4`). **HP lives on the owning peer only**: a peer reads 0 in a remote unit's
  `+0x108` (and 1.0 in `+0x104`) whatever it has taken, so read a unit's HP where it is owner 0;
  the firer's peer computes a hit and sends it to the owner as a `0x0B`. Tab opens the options
  panel and does not pause a network game.
- **Each peer draws its own units in player 0's colour** on the Vulkan lane; the engine's own frame
  is right. A renderer limit (`gpu-status.md` §3.2), not a network fault.
- `SELPROV`'s `SELECT` crashes the game on the non-TCP/IP rows; select *Internet TCP/IP
  Connection For DirectPlay* **by name, never by row number** — it is row 0 under wine's builtin
  DirectPlay and row 3 under the native one.

## Many unit types: the synthetic mod

The build has 16 384 unit-type slots, stock 512 (`research/notes/tadr-port/content-ids.md`).
`tools/unittypes_fixture.py` writes a mod of N generated kbot types, `SYN00001`…, each an FBI of
its own on a stock model and one small compiled script:

```bash
tools/unittypes_fixture.py tagpu/instances/u1/gamedir/zzsyn.ufo --types 16105 \
    --builder ARMLAB --download 12 --ctrl G      # 16 105 reaches ID 16 383, the ceiling
rm -f tagpu/instances/u1/catalogue.json          # the cached type list is now stale
tools/tacli log u1 -g "enginefix|limits"
```

- **The download buttons are not on a numbered page.** A builder's download entries live on the
  `ARMDL` GUI pages, which `ui --page` does not reach: open the builder's menu and walk with
  `ARMNEXT`. `--builder ARMLAB` puts them in the Kbot Lab, which builds kbots; a commander's
  download button for a kbot arms nothing.
- `--ctrl G` gives the types the category `CTRL_G`, so Ctrl+G selects exactly them.
- **A network game needs the same `.ufo` in every peer's gamedir.** The battle room shows
  SYNCHING while the joiner sends its types, 64 a tick: about 12.5 s at the ceiling the first
  time, under 2 s on a rejoin.
- `--raw-keys` gives thousands of types one unit-sync key, which stock never ends the join on;
  the build re-keys them at load and logs `unit sync keys: N of M types re-keyed`. Stock content
  logs no re-key. `--part K/N` writes every Nth type from the Kth, so a mod splits over archives
  whose types interleave: two peers holding the parts under swapped archive names load the types
  in another order, and must still agree on every key. `--pad KB` inflates every script so a mod exhausts the address space: the
  out-of-memory message's test.

## Weapons past 256: the test weapons

The build has 4096 weapon IDs, stock 256 (`research/notes/tadr-port/content-ids.md`).
`tools/weaponids_fixture.py` writes stock weapons under new IDs, each on a copy of a stock tower
(the docstring lists them): 4000, 5000 and one with no `ID=`; lasers 253–255 and 3581–3583; slow
targetable rockets 250 and 3322, one low byte; and `WIDAMD`, an interceptor at 3000. `--low`
keeps the IDs below 256, for a build without the raise.

```bash
tools/weaponids_fixture.py tagpu/instances/w1/gamedir/zzwid.ufo
tools/tacli log w1 -n 1000 -g "enginefix: weapon"      # 5000 and the ID-less one: skipped
```

- **An interceptor fires only from stock**, and stock is built only by an order from the unit's
  build page: `tacli click` the unit (it must be on screen), then `tacli ui <i> click ARMMAKEANTI`
  once a missile. `tacli weapons <i>` shows the count as `stk=`.
- **A weapon reaches a feature through its blast**: area damage hits the feature cells inside its
  radius, so the lasers carry a blast of 96; a stock laser's 8 does not reach a wreck's cells.
- **Place a test on level ground.** A tower below a cliff aims at a target on top and never fires
  (`tacli weapons` shows it aiming, `rl=0`), and a unit placed at height 0 stands in the sea.
  Read the height at `*(*0x511DE8+0x14287) + (y/16 · width + x/16) · 13 + 4`, one byte, with the
  width at `*0x511DE8+0x14233`.
- **In a network game a feature's damage is the host's.** A peer that is not the host sends each
  hit to the host as a `0x0F` and the host damages the feature; an outcome (destroyed, burned,
  reclaimed) goes out as a `0x0F` from whichever peer ran it. Compare the
  peers paused, after the fire stops and every projectile has landed: a projectile in flight at
  the pause is damage one peer has applied and the other has not.
- **An interceptor's detonation reaches only a projectile still in flight** on the peer that owns
  it, which is ahead of the host's copy by the link's delay: a fast rocket caught near its target
  has often already hit there. The fixture's rockets are slow for that reason.

## The new data keys: the key fixtures

TADR section C's keys (`research/notes/tadr-port/data-keys.md`). `tools/datakeys_fixture.py`
writes stock units under new names, each carrying a key; for the ghost's `PreviewPieces=`, four
ARMLLT clones on the Commander's fourth build page (the docstring lists them).
`scenarios/ghost-mask.json` is the ghost's oracle: four finished structures whose `Create()` hides
a piece, and the builders that place their ghosts.

```bash
tools/datakeys_fixture.py tagpu/instances/c1/gamedir/zzkeys.ufo
tools/tacli arm c1 'ghost.on=alpha=1.0'   # with the default arm set; opaque, so the silhouette reads
tools/tacli scenario load c1 ghost-mask
tools/tacli log c1 -g datakeys            # every key read at load, every mask as it is computed
```

- **`maskmiss=` and `maskcut=` in the ghost heartbeat must stay 0**; `masked=` counts the ghosts
  drawn with a mask.
- **A queued site's ghost shows only with the order overlay**: select the builder and hold shift
  (`keys <i> down:shift`, shield on).
- **Give `ui click <TYPE> --page <n>` the type's own page.** Walking the pages until a click
  succeeds ends on a download page whose name carries no number (`ARMDL`), which `--page` cannot
  page away from; `ui click ARMPREV` returns to a numbered one.
- **`+reload <unit>` re-reads one type's FBI and COB mid-game, and runs only with `tacli switches
  <i> cheats=on`**: the chat grants the debug run level only with that bit. **It kills every unit
  of that type first** — a side left with no unit ends the game on `ENDMSN.GUI` — so keep another
  unit per side, and spawn the type again with `scenario apply` to test the reloaded def.
- **Type into the chat bar four characters per `keys` call, with a `tacli shot` between calls,
  and read the line back**: a long `char:` run loses characters, and the first batch after the
  `return` that opens the bar can be lost too. Clear a wrong line with `backspace`.

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
  `tagpu_classicpp.cfg`, `tagpu_ss.off`, `tagpu_fps.on` and `tagpu_hud.on/.off` each beat the
  store for what they name; `tacli arm` drives them for A/Bs. **Display mode, Monitor and Frame
  cap have no lever and are live under `--defaults`**: tacli writes the instance's placement into
  the store on every launch, so a click there lasts one session and the next launch puts the
  instance back on its tile.
- **Under `tagpu_defaults.off` (every launch without `--defaults`) the store has no say and every
  row but Shadows is greyed** — except that the placement (`display`, `window`, `maxfps`) is read
  under every launch. Shadows is then the engine's own shadow switch. Test the menu with
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
  first-run signal: it renames the files an older menu wrote to `*.migrated` and leaves
  `impure-migration.txt`, after which a missing store only gets the defaults. To test the
  migration, delete the store and the record, then launch the exe by hand in the gamedir
  (`WINEPREFIX=<prefix> DISPLAY=<d> WINEDLLOVERRIDES=ddraw=n,b wine TotalA.exe`) on a private
  Xvfb — it goes fullscreen. `tacli launch --shipped` on a fresh instance is the same first run.

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
  logged as `frame cap: Refresh = N fps`); a tacli instance's `--maxfps` is written over it at
  every launch.
- **At most eight devices are listed** (our cap; a stage button's art index is clamped by the
  engine, so a row past four stages draws the four-bar plate and still works). Names are truncated
  to 31 characters at a word boundary.
- **A dialog over the world at zoom ≠ 1 takes 1:1 clicks** (the transform reads the engine's
  ownership bit at `main+0x37EBE`), so the rows are driven at any zoom; `SHARE.GUI` is the one
  screen whose zoomed clicks are a known gap.

## A remote Windows machine

A native Windows run is the test for anything Wine hides: a driver's formats and extensions, the
real `ddraw.dll` loader, a player's folder. A **remote instance** is a test folder on that
machine, driven over SSH by the ordinary verbs. The machine is somebody's desktop: ask before
using it, and never start or stop a game you did not launch.

```bash
tools/tacli remote add w1 --ssh <user>@<host> --key <private key> --from '<player folder>'
tools/tacli arm w1 gui.on                         # arm files go into the test folder
tools/tacli scenario load w1 marker-mix --res 1920x1080   # launch -> menus -> live -> applied
tools/tacli ui w1; tools/tacli keys w1 ctrl+a; tools/tacli eye w1 1700 1640
tools/tacli ab w1 gui                             # capture one pass, fetched to tagpu/instances/w1/ab/
tools/tacli log w1 -g 'vk: (census|shot)'
tools/tacli stop w1                               # stops the game
tools/tacli rm w1                                 # deletes the test folder and the task
```

- **`remote add` copies the player's folder once** into its own test folder (default
  `<user profile>\tacli\<name>`, or `--to`) and never writes the player's folder. It compares
  the two folders as the file system names them, and refuses a junction, a short name or a
  `subst` drive that would put one inside the other. It refuses while any `TotalA.exe` runs
  there, and when the console user is not the SSH user. The address, the account, the key and
  both folders live only in `tagpu/instances/<name>/instance.json`, which is gitignored; never
  copy them into tracked content. An add that failed half-way is removed with `tacli rm`. Its
  last step seeds the test folder's registry store by reading the player's key (below).
- **The verbs a remote instance answers**: `launch`, `stop`, `rm`, `arm` (and its `=off`), `keys`,
  `ui`, `eye`, `shield`, `scenario load`, `log`, `ab` and `crash`. Every other verb refuses
  before it touches anything. `tacli ls` lists a remote instance without contacting the
  machine. An `instance.json` that does not read is an unusable instance: every verb refuses it,
  `rm` included, until it is fixed or removed by hand.
- **`launch` refuses beside any `TotalA.exe` it did not start** (it may be the player's game).
  It uploads this tree's `ddraw.dll` and checks its MD5 (`--keep-dll` keeps the one there), and it
  writes the harness files a local launch writes, the shield included. It reads the test
  folder's store first and refuses one that is missing or that the DLL would not load, and it
  refuses a DLL that does not fail closed. `--arg` refuses any switch whose character after the
  dash is `r` or `d` (the engine reads `-register` as `-r`). A launch succeeds only when the
  run's log says the store is served; a refused run fails it with the DLL's own line.
  `--res`, `--maxfps`,
  `--map`, `--player`, `--los`, `--mapping`, `--unit-limit`, `--defaults` and `--sound` work as
  locally. `--window`, `--display`, `--slot`, `--dplay`, `--intro` and `--shipped` are refused.
- **A test folder's registry is a file; TA's settings key on that machine is never written.**
  `tacli-state\registry.txt` holds TA's key. Every remote launch passes the token
  `-xtacli-test`, which the engine skips; with it, or with a `tacli-state` folder beside
  `TotalA.exe`, the DLL answers the registry calls of `TotalA.exe` and `win32.dll` from the file,
  and any other key is read-only. `launch` puts its values (sound off, `Interface Type`, the
  display and skirmish values) into that file. The log's second line says which mode ran:
  `registry: TEST MODE, entered by the -xtacli-test token and the tacli-state folder -- …
  hooks: TotalA.exe 9 of 9 registry imports, …`. A test launch whose store is missing or does
  not load logs `…, but <what>: the game is not run` and ends at once. A player's own folder
  has neither signal and logs `registry: real (…)`. Nothing is restored after a test, and a
  test killed at any moment leaves the player's key as it was. What no hook reaches (the Task
  Scheduler's records of the task, the system DLLs) is listed in `tacli-design.md`.
- **The game runs from the instance's scheduled task**, `\tacli\<name>`: TotalA.exe itself on
  the console user's desktop, at normal priority. `launch` reports `TotalA.exe runs at priority
  Normal`. A task that starts no game fails the launch with the task's own result code. `rm`
  removes the task, and the task folder `\tacli\` once no other task is in it.
- **The shield is on**, as locally: that desktop's keyboard and mouse do not reach the game,
  and `tacli shield w1 off` hands it over. With the player's `impure.cfg` the game opens
  fullscreen on that machine's primary monitor; `--res` at the monitor's own size avoids a
  mode change.
- **Nothing tacli replaces is lost.** Before a file of the player's copy is first replaced or
  deleted in the test folder, its original, as `remote add` copied it (by SHA-256), is kept
  beside it as `<name>.tacli-original`.
- **Comparing with the reference setup.** The store is the player's key, so the skirmish values
  are theirs: pass `--los 0 --mapping 1`, the reference prefixes' values, or line-of-sight fog
  and unmapped ground differ between the machines (`--mapping 0` blacks the terrain out). Health
  bars need `damagebars` 1 in the store, and no launch flag sets it; a stopped instance's store
  is changed through `_remote_store_update` in `tools/tacli`. Nothing writes `tagpu_zoom.txt`
  there, so the captures are at 1×. `ab` refuses a frame two passes drew into: the effects alone
  need `fx.on=nomodels`, since their 3D models are the unit pass's. A shadow cannot be captured,
  because a one-pass frame has nothing under it.
- **Read the result with `log`**: the `vk:` lines name the device, its depth format and each
  missing extension; each refusal says which pass stood down and why; the `vk: census` line says
  which passes drew. `crash` reads the test folder's `ErrorLog.txt`.
- **Every statement is one line of PowerShell**, made by `ps_script` in `tools/taremote.py`.
  PowerShell reading stdin skips a line that does not parse as one statement, with only a
  parser error on stderr, and `ps_script` makes
  every line after a skipped one run nothing. Add remote operations there, through that
  function, never as a hand-written script. How the link, the routing, the store and the task
  work: `research/notes/tacli-design.md`, "Remote instances".
