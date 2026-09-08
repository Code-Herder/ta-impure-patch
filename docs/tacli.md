# ta-cli

ta-cli is a command line tool that drives Total Annihilation. I wrote it because developing a renderer inside a 1997 game means launching the game hundreds of times, clicking through the same menus, getting the same units on screen and looking at the same spot, and I didn't want to be the one doing that. An agent can do all of it from a terminal now, and so can a script.

It lives at `tools/tacli`. It's plain Python 3 with no dependencies, and every command can answer in JSON with `--json`, so an agent never has to parse a screenshot to know what happened.

There are two ways I use it:

- **With an agent skill.** The `ta-drive` skill teaches an agent the whole loop: build the DLL, launch the game, put a situation on screen, look at it, measure, fix, repeat. I describe the feature, the agent drives the game.
- **As a test runner.** A scenario file plus a couple of tacli commands is a test. Write it once, rerun it whenever you touch the code.

## Instances

Every game tacli launches is an isolated instance. It gets its own game directory (a symlink mirror of your install plus its own config, trigger and log files), its own wine prefix, its own wineserver and its own window. Several run in parallel, tiled on the real desktop, and you keep using the PC while they do. They are silent by default, skip the intro movies, and run windowed at whatever resolution you ask for.

The patch's `ddraw.dll` is copied into the instance at launch, so rebuilding the DLL never touches a game that is already running. Relaunch and you get the new build.

The shipped DLL turns every play pass on by itself. An instance is a lab bench, so tacli writes `tagpu_defaults.off` into it and only the arm files count, which keeps a bare launch a stock control. `--defaults` on `launch` or `scenario load` gives the instance the player's configuration instead, and it sticks until `--no-defaults`.

```bash
tools/tacli launch t1 --res 1024x768      # created on first use, a window in about 2 s
tools/tacli ls                            # names, pids, windows
tools/tacli stop t1
tools/tacli rm t1                         # when it has no further use
```

One instance can run stock TA as the control and another the full stack, side by side. That is how most A/B measurements in the wiki were taken.

## Driving the game

**Menus by name.** `tacli ui` reads the game's own gadget tree from inside the process, so it sees what the engine sees: every button, list, slider and toggle on the current screen, whether it is active, and what stage it is on. You snapshot the screen, then act on a gadget by name. Every action waits for the gadget to exist and be clickable, then reports what happened: the screen it moved to, the stage a toggle went to, or the engine's own confirmation. No pixel hunting, no screenshot-and-guess.

```bash
tools/tacli ui t1                         # what is on screen right now
tools/tacli ui t1 click Skirmish
tools/tacli ui t1 select MAPNAMES 'Anteer Strait'
tools/tacli ui t1 set LineOfSight 2
tools/tacli ui t1 fill GAMENAME Test_2
tools/tacli ui t1 wait --gui SKIRMISH
```

**Keys and clicks.** Injected inside the process through the patch, not through X. Menu accelerators, hotkeys, modifier combos like `ctrl+d` or `shift+2`, clicks at game coordinates, the mouse wheel. It works whether the window is focused or not.

**Orders in world coordinates.** `tacli order` names the order and the target outright and hands it to the engine's own order constructor. There is no screen in it anywhere, so zoom, resolution, the camera and fog don't matter. Order everything selected, or one unit by index wherever it is.

```bash
tools/tacli order t1 --sel move pos 1988 1716
tools/tacli order t1 --unit 2 --expect ARMCOM attack pos 3160 1200
tools/tacli order t1 --unit 2 reclaim pos 1700 1500
tools/tacli order t1 --unit 2 stop
```

The vocabulary is TA's: `move attack guard repair patrol reclaim capture load unload blast stop mobilebuild`.

**Camera and zoom.** `tacli eye` holds the camera at a world position. `tacli wheel` turns the mouse wheel, which is the zoom control once the patch's zoom is armed.

**The input shield.** On by default. The game ignores the real keyboard and mouse, so an agent's game can run next to you without your typing landing in it, and its injected input never reaches your desktop. `tacli shield t1 off` hands the game over when you want to play the situation yourself.

**The patch's features, per instance.** Every feature of the Impure Patch is a trigger file in the game directory, and `tacli arm` writes them for one instance: the OpenGL passes, the zoom, Classic++, the GL UI, the extra weapons, the COB trace. Arm a pass before the launch you want it in.

```bash
tools/tacli arm t1 'native.on=all wrecks' terr.on feat.on fx.on sfx.on mark.on zoom.on vpwide.on gui.on
tools/tacli arm t1 classicpp.on
tools/tacli launch t1 --res 1920x1080
```

## Looking at the game

- `tacli shot` — the engine's own 8-bit surface. Engine truth, and the only view that shows the engine's UI.
- `tacli glshot` — the OpenGL framebuffer, what is actually on screen, including everything the patch draws.
- `tacli roster` — the live units with type, owner, world and screen position, plus where the camera is.
- `tacli log` and `tacli wait` — the patch's log, and a wait for a regex in it. This is how a script knows a pass armed, a model loaded, or a restore finished.
- `tacli peek` — read game memory from inside the process. The cheap way to answer "did that actually change anything" without a debugger.
- `tacli crash` — the last crash TA recorded, with the fault address symbolised. Every launch and wait polls for it and fails fast with the address instead of sitting out a timeout.
- `tacli weapons` — dump the weapon slots of any unit. The oracle for the extra weapons work.
- `tacli units`, `tacli features`, `tacli maps` — ask the running game what unit types, features (that is where wreck names live) and maps it has. Cached per instance.
- `tacli switches` — read or set the engine's debug bits live: `shootall`, `noshake`, cheats, etc.

## Scenarios

A scenario is a JSON file that describes a game scenario: the map, the sides, the units and where they stand, the camera. One command takes a fresh game from the menus into that live scenario, so a test or a measurement starts from the same state every time instead of from a sequence of clicks.

```bash
tools/tacli scenario load t1 200v200      # launch, menus, map load, 400 units spawned, camera placed
tools/tacli scenario apply t1 one-wreck   # add a situation to a game that is already running
tools/tacli scenario validate 200v200 --instance t1
tools/tacli scenario list
```

It is not a savegame. TA's `.sav` is binary, index-keyed and never fully reverse engineered. Instead the patch spawns what the file describes by calling the engine's own creation functions, the same ones the game uses. Everything is named, never numbered: unit types, wreck types, maps and switches are resolved against the live game, and the file is validated three times (schema, the instance's catalogue, then inside the game) before a single unit is created. A typo creates nothing rather than half a scenario.

The options are pretty comprehensive:

- **Setup**: map, resolution, unit limit, the players (human, AI or off, side, colour, starting metal and energy), engine switches, and whether to clear the skirmish's starting commanders.
- **Units**: type, owner, position, facing, height, health, nanoframe percentage (a half-built look), stance (hold, manoeuvre, roam) and initial orders.
- **Groups**: a composition of types and counts laid out in a grid, a line or a random scatter around a centre, seeded so it comes out the same every run. Any per-unit attribute can sit on the group.
- **Features**: wrecks and other map features, by name, with a facing.
- **Orders**: move, attack, guard, patrol, reclaim and the rest, to a position or to another named entity. That is how a wreck gets reclaimed on cue.
- **Camera**: centred on a named unit or feature, or at a position.
- **Names**: every entity has an `id` you choose. The load report gives back the engine index of each one, so a test can track a specific unit by its own name instead of guessing.

```json
{
  "format": "ta-scenario/1",
  "description": "One commander at a named spot",
  "setup": { "clear_existing": false, "switches": {"shootall": true, "noshake": true} },
  "units": [ { "id": "hero", "type": "ARMCOM", "owner": 0, "pos": [1600, 1600], "facing": 90 } ],
  "camera": { "center_on": "hero" }
}
```

`load` is the whole trip and always a clean start. `apply` stacks a situation onto a running game, so an agent can say "now add 50 more bombers" to a fight in progress. Explicit flags win over the file, so one scenario re-runs on another map or resolution without being edited.

## Multiplayer, locally

Two instances can play one game over loopback: one hosts, one joins, and `tools/mp_lobby.sh` drives both lobbies through to a live game. A scenario applied on the host shows up on the joiner through the game's own network path. That is how the extra weapons were proven in multiplayer without a second PC, and it's the setup for anything that has to be checked on both sides of a game.

```bash
tools/tacli launch h1 --dplay --free-dplay-port     # the host
tools/tacli launch j1 --dplay                       # the joiner
tools/mp_lobby.sh h1 j1 'Two Continents'            # menus -> battle room -> live
```

## The agent skill

`.claude/skills/ta-drive/SKILL.md` is the skill. It is what an agent reads before it touches the game, and it is where the lessons accumulate: every time an agent lost an hour to something, the fix went into the skill so the next one doesn't.

The rules of engagement:

- **One instance per session**, named after the task. Never drive an instance you didn't launch, another agent may own it.
- **All input goes through tacli.** Never xdotool, never move the human's pointer, never touch the display to test something. That is their desktop.
- **Silence is the default.** Sound only when asked.
- **Arm every pass before launching**, unless the launch is a measurement. A bare launch is the 1997 renderer, which is a control, not a demo.
- **Clean up.** Stop the instance when done, remove it when it has no further use.

Then the loop: launch, snapshot the UI, act, order, look (`shot`, `glshot`, `roster`, `log`), measure (`peek`, `weapons`), stop. The rest of the file is the accumulated detail: how the input path works and where it bites, how to read the catalogues, how to arm and read every pass, the multiplayer recipe, and a list of things that will bite you. Screenshots, video and frame-by-frame work are the `ta-capture` skill.

## Scenarios as tests

A test is a scenario, a few tacli commands and an assertion on what they return. Some real ones:

- The nine `cob-*` scenarios put one unit of each class through its script (walk, aim and fire, die, take off, dive, etc.) with the COB trace armed. `tools/cobtrace_fixtures.py` runs all nine and keeps the traces as fixtures for the COB editor's virtual machine.
- `warlordex-groundattack` gives one ship a ground attack order with nothing else in range. `tacli weapons` reading fire counts on slots 4 to 7 is the whole test of whether the extra weapons obey a ground order.
- `hires-crowd` puts twelve ordinary unit types on screen before the one with a replacement mesh. Pass is one line in `tacli log`. A two-unit scenario could never see the bug this one caught.
- `shootall-ab` is six idle Peewees, an enemy building in range and one engine bit. Flip it, wait, count wrecks.

```bash
tools/tacli scenario load t1 hires-crowd
tools/tacli wait t1 'pose bound 12 of 12' --timeout 30
tools/tacli glshot t1 -o /tmp/crowd.ppm
tools/tacli stop t1
```

## Example scenarios

Everything in `scenarios/` was written to answer a question. What each one was for:

**The renderer**

| scenario | used for |
|---|---|
| `200v200` | 200 ARM vs 200 CORE on Two Continents. The performance bench and the first thing to load after any renderer change. |
| `one-unit` | One commander at a named spot. The smallest live proof that the applier works. |
| `one-wreck` | One wreck and no units. Features go into the engine through a different door. |
| `fx-lasers` | A long laser duel between a commander and three laser towers. The laser beam pass. |
| `fx-rockets` | Rockos lobbing rockets at a fusion plant for minutes. The 3D projectile pass, thrust flame and ground shadow. |
| `fx-mix` | Lasers, rockets, AA missiles and a solar to blow up, all in one frame. The effects mix. |
| `sfx-smoke` | Badly damaged buildings, nanoframes to repair. Damage smoke and nanolathe particles in a forest. |
| `sfx-strait` | The same on a coast, plus boats and a submarine crossing. Wakes, bubbles, burning debris. |
| `feat-forest` | A dense forest with units parked behind the trees. The occlusion proof for the feature pass. |
| `exit-sort` | A kbot standing on its lab's own tiles. Unit versus structure sprite sorting against the stock engine. |
| `waterline` | Commanders leg-deep and waist-deep on a shore. The waterline cut. |
| `shadow-struct` | Own and enemy buildings at 1x and zoomed. Structure shadows over our terrain. |
| `shadow-lab` | A kbot lab on the shore. The stock engine's structure shadows beside ours. |
| `shadow-mix` | A ground unit, a building, a hovercraft and a wreck. Shadow dedupe. |
| `shadow-air` | Five aircraft patrolling lanes across a coastline over a row of ground units. Aircraft shadows. |
| `shadow-air-fx` | Aircraft crossing burning buildings on a known lane. Whether shadows composite above live effects. |
| `hires-one` | One Peewee with a replacement mesh beside an engine-drawn AK, at facing 45. The yaw regression test. |
| `hires-peewee` | 20 Peewees with the mesh against 20 AKs. The replacement mesh in a crowd. |
| `hires-crowd` | Twelve ordinary types on screen before the one with a mesh. The replacement slot regression. |
| `hires-wreck` | Four replaced units and four husks. Wrecks must not inherit a replacement mesh. |

**The browser lab (tascene)**

| scenario | used for |
|---|---|
| `tascene-parity` | A fixed camera, two still units, nothing that moves or fogs. The pixel-for-pixel parity fixture between the engine and the browser. |
| `tascene-parity-core` | The same with the human on the CORE side, so the CORE panel art goes through the GL UI. |
| `tascene-base` | A small ARM base on open grass with a hill for slopes. The scene to judge lighting and shadows in. |
| `tascene-air` | The base with five aircraft parked in the air at each stock cruise altitude. Shadow drop against altitude, read off one still. |

**Extra weapons**

| scenario | used for |
|---|---|
| `wpn-peewee4` | Six four-gun Peewees in range of an enemy solar. The first proof of a fourth weapon slot. |
| `wpn-llt10` | Two ten-laser towers. Ten weapons on one unit. |
| `wpn-badtgt` | The same gun in slots 1 and 4 with different target masks. Whether a slot reads its own mask. |
| `warlordex-lasertest` | A WarlordEx with six Skeeters parked inside its new battery's range. Do slots 4 to 7 fire at all. |
| `warlordex-groundattack` | One ship, nothing to auto-acquire, a ground attack order. Do the extra slots obey a ground order. |
| `warlordex-vs-fleet` | A WarlordEx ringed by 40 Skeeters. The extra battery in a real fight. |
| `warlordex-vs-warlord` | Two WarlordEx against two stock Warlords. The upgrade against the original, driven by hand. |
| `warlordex-duo-vs-200` | Two WarlordEx against 200 Skeeters, for playing by hand with the shield off. |
| `los-acquire-probe` | A ship with a huge sight distance and targets parked in fog inside its range. What the engine actually acquires, and how far line of sight really reaches. |
| `shootall-ab` | Six idle Peewees, an enemy building in range, one engine bit. What the `shootall` switch does. |

**COB scripts (the editor's fixtures)**

| scenario | used for |
|---|---|
| `cob-kbot` | A Peewee walks 300 units and stops. The walk cycle. |
| `cob-tank` | A Stumpy on hold with an enemy in sight. Aim, fire, recoil, damage smoke. |
| `cob-building` | A wind generator alone. Activate, deactivate, the blade spin from the engine's wind. |
| `cob-death` | A Peewee at 5% health beside an enemy tower. The death script and the corpse type. |
| `cob-fighter` | A Hawk patrolling towards a transport. Takeoff, banking flight, the first air-to-air shots. |
| `cob-gunship` | A Brawler attacking an AK. Hover, continuous aim, fire. |
| `cob-bomber` | A Thunder bombing a solar. Takeoff, the run-in, bomb release, the circle back. |
| `cob-ship` | A battleship against a destroyer. Multi-turret aim and fire, wakes. |
| `cob-sub` | A submarine attacking a destroyer. The dive, the torpedo aim, the bubbles. |

## Commands

Every command takes an instance name first and `--json` anywhere.

| command | what it does |
|---|---|
| `create` | create an isolated instance (`--res`, `--display`, `--slot`, `--sound`, `--intro`, `--no-shield`) |
| `launch` | start an instance, creating it if needed. Skirmish presets `--map`, `--unit-limit`, `--player`, `--los`, `--mapping`; extra engine switches with `--arg`; `--dplay` for multiplayer; `--title` for the window label |
| `ls` | list instances: names, pids, windows, displays |
| `stop` | stop an instance (`--hard` also kills its wineserver) |
| `rm` | delete an instance |
| `keys` | send key tokens: `space`, `return`, `esc`, `ctrl+d`, `shift+2`, `k:a`, `mouse:X,Y`, … |
| `click` | click at game coordinates (`--right` for the order button) |
| `order` | give units an order in world coordinates: `--sel` for the selection or `--unit` for one unit, with `--expect` to guard against a recycled index |
| `wheel` | turn the mouse wheel, the zoom control (`--at X Y` to point first) |
| `eye` | hold the camera at a world position (`--release` to let go) |
| `ui` | snapshot and drive the on-screen gadgets: `show`, `click`, `set`, `check`, `uncheck`, `select`, `hover`, `fill`, `press`, `wait`; `--page` walks a paged build menu |
| `shield` | the input firewall: `on`, `off`, or omit to report |
| `arm` | set or clear the patch's trigger files, e.g. `native.on=all owndraw.on zoom.on classicpp.on`, `native.on=off` |
| `gui` | the GL UI renderer: `on`, `strict`, `census`, `off`, `remove` |
| `switches` | read or set the engine switches: `shootall=on noshake=on` … |
| `shot` | screenshot of the engine's surface (PNG) |
| `glshot` | capture of the GL framebuffer, including the patch's passes (PPM) |
| `roster` | the latest unit roster and the camera eye |
| `log` | tail the patch's log (`-n`, `-g regex`) |
| `wait` | wait for a regex in the log (`--timeout`) |
| `peek` | read game memory: `'*0x511DE8+0x2C74:2'` (deref with `*`, `+hex` offsets, `:1|2|4|s<N>|x<N>`) |
| `weapons` | dump the weapon slots of units, all live units by default |
| `crash` | the last crash TA recorded, symbolised |
| `units` | the live unit type catalogue |
| `features` | the live feature catalogue, where wreck names are |
| `maps` | the map names, read off the skirmish map list |
| `catalogue` | what this instance has cached |
| `scenario list` | the scenarios in `scenarios/` |
| `scenario validate` | schema and catalogue checks, no game needed |
| `scenario expand` | the flat entity list a file expands to, no game needed |
| `scenario load` | launch a game and put the situation in it (`--map`, `--res`, `--unit-limit`, `--los`, `--mapping`, `--restart`) |
| `scenario apply` | apply a situation to a game that is already running |
