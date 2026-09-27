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
tools/tacli log w1 -g "weapons: (unit-data|loader|reload|VIOL|MISM)"   # each load empties, then fills
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
tools/tacli launch h1 --dplay --dplay-port 47731 --free-dplay-port   # the host
tools/tacli launch j1 --dplay --dplay-port 47731                     # a joiner, as many as nine
tools/mp_lobby.sh --map 'Two Continents' h1 j1      # menus -> battle room -> live
MP_NO_START=1 tools/mp_lobby.sh h1 j1               # stop in the battle room
tools/mp_leave.sh h1                                 # surrender -> main menu; a host's ends it for all
```

- `--dplay` installs native DirectPlay into that instance's prefix and appends the overrides to
  `ddraw=n,b`. Sticky per instance; a single-player instance keeps wine's builtin. **The NAT
  helpers are refused** (`dpnhpast,dpnhupnp=d`): the transport skips a helper that does not load,
  and one that loads opens a UDP socket on every interface of the machine for UPnP discovery and
  asks the LAN's router to map each game's ports. The games meet on loopback and need none of it.
- **One DirectPlay port per game, several games at once.** DirectPlay's name server
  (`dplaysvr.exe`) binds its port for the whole machine, and the TCP/IP transport sends every
  session enumeration to it, so two games on one port collide however separate their prefixes
  are — joiners stuck on SELGAME with JOINGAME grey, enumerating the other game's server.
  `--dplay-port N` patches N into the prefix's copies of both files (`tools/dpport.py`: seven
  immediates, refused on any build it does not know) after every `--dplay` reinstall. Give all
  peers of one game the same N and every game a different one; the joiner's address stays
  `127.0.0.1`. `tacompat.py` takes 47625 upward for its own games, so pick from 47700 up by hand,
  and check `ss -tulnp 'sport = :N'` is empty first. Default and `--dplay-port 47624`: stock.
- `--free-dplay-port` kills a stale `dplaysvr.exe` holding the instance's own port. The server
  outlives the game that started it, so a leftover one makes the next host on that port fail
  `Open(DPOPEN_CREATE) = DPERR_GENERIC`, which looks exactly like a broken prefix. Put it on the
  **hosting** launch only: a peer's game on the same port is that host's. Servers on other ports
  are left alone, and never `pkill -x dplaysvr.exe` by hand — that takes every game down.
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
  every roster at once. For a stress fight, `tacli registry <host> MultiCommanderDeath=0` before
  the host launches and read `*511DE8+37EF6:1` as 0 on every peer; the value is the host
  instance's own.
- **To compare peers, pause and read both rosters.** `tacli keys h1 shift pause` sends the game's
  own `Pause` (the leading token is the one a `keys` call drops), which pauses every peer; confirm
  with `tacli peek <i> '*511DE8+38A51:1'` reading 1 on each. Then `tacli roster <i> --json` on
  every peer (`tacli units` lists unit TYPES, not live units): match units by `engine_index`,
  which is the same slot on every peer, and remember `owner` is each peer's own seat numbering.
  Stationary units agree exactly. A moving unit on a remote peer is where its owner last reported
  it, so compare against the owning peer (the one where the unit is owner 0) and scale the
  tolerance by the unit's speed; the pause itself lands a few ticks apart (`GameTime`,
  `*511DE8+38A47:4`). **HP lives on the owning peer only**: a peer reads 0 in a remote unit's
  `+0x108` whatever it has taken (and 1.0 in `+0x104` until the owner's round robin writes it,
  26–37 s after a create), so read a unit's HP where it is owner 0;
  the firer's peer computes a hit and sends it to the owner as a `0x0B`. Tab opens the options
  panel and does not pause a network game.
- **Kill counts can differ between peers.** A peer counts a kill only when its own copy of the
  victim reads `+0x104` = 0.0, and another peer's copy of a new unit reads 1.0 for 26–37 s after
  its create, so a unit killed sooner (a scenario's victims, typically) counts only on its owner's
  peer. Wait that long, or read kills on every peer. A scenario's `kills` is set only on the peer
  that applies it.
- **Stale hits: a hit names its victim by slot.** To make one land on a reused slot, arm
  `dmgdelay.on=30` on every peer (the attacker's are the hits delayed, and a receiver counts
  `young` only while armed), apply
  `scenarios/b4-guns.json` (laser towers and two fusion plants, Town & Country) on one peer and
  `scenarios/b4-victims.json` (four ARMCK on hold) on another every two seconds, and read the
  `hits:` section of the `packet:` heartbeat on each (`references/levers.md`): `young owner=`
  counts hits the owner applied to a unit younger than the delay, and must read 0. The victims'
  owner is the owner; a third peer is the bystander, whose undecidable applies (`by=S/U/R`) can be
  young without being stale.
- **The ghost commander: the peer that loads last refuses the others' first creates.** On two peers
  by `tools/mp_lobby.sh`, peek every commander's slot on each peer from the first in-play tick —
  the first slot of each player's block, `[player+0x67]`; records are in local order and blocks
  in DirectPlay-id order, so a record's index is not its block's — and read the `ghost:` section
  (`references/levers.md`): the late peer holds creates refused during its load (`q=`) and
  replays them before its first tick, and makes one refused in a catch-up tick at once (`now=`,
  a `created slot … at GameTime 1, in a catch-up tick` line — where the host's commander lands
  in a two-peer start). Order the host's commander to move the moment it is in play to exercise
  the dirty create; `ghostq.off` on the late peer leaves that half alone (`pos ground=1`,
  `unbound=0 short=0`). Its ground position is point 0 of the owner's path — a straight move's
  origin until the unit is within 5 px of its goal — so it lands on the host's commander only
  while that one has just set off.
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

### The weapon keys

The same fixture carries the weapon keys' clones: stock weapons under new names and IDs
(230–244, 247–248; 245 and 246 are the burning features' weapons), the towers, subs,
hovercraft, commanders and transport that fire or carry them, and two hail maps. Its docstring
lists every one. Four scenarios, each pairing a keyed unit with its control:

| scenario | what it shows | how it is driven |
|---|---|---|
| `c2-weapon-keys` | every targeting key, the AI's towers against the player's units on hold | runs by itself (`shootall` on); read the roster and the log |
| `c2-order-cursor` | the right-click's order and the attack cursor per key | select the player's tower, park the pointer on a target, `click --right`; a sonar sees the submerged target |
| `c2-surfacefire` | Escalation's two `surfacefire` shapes | `order --unit N --expect WKCOMSF blast unit T` for the D-guns, `attack unit` for the subs; `switches <i> radar=on` for sight |
| `c2-nomapalert` | `nomapweaponalert` under keyed hail | `--map "WK Hail C"` gives the same hail without the key |

### Veterancy and the four stock defects beside it

The same fixture carries veterancy's types: `VTLLT0` (no key), `VTLLT1` (a level a kill),
`VTRATE0`, `VTBAD1..8` (each refused at load, with its reason in `log -g datakeys`), the unarmed
`VTTGT0/1`, `VTCOM0/1` and `VTKROG0/1`, `B7HUGE` (a laser past the HP word), and the map
`WK Hail T` (targetable hail). A scenario sets a unit's kill count with `"kills"`.

| scenario | what it shows | how it is driven |
|---|---|---|
| `c3-veterancy` | every effect beside its stock control: dealt, taken, reload, lead, spread, the capture's cost, a unit reclaim's step | runs by itself; read HP over time |
| `c3-reclaim-bound` | a keyed reclaim step held to what its product holds, beside stock's | read the two CORKROGs' HP; the keyed reclaimer starts about a minute in |
| `c3-kill-lines` | "10 kills - Vet10" and the three other lines | hover each tower (`keys <i> pmove:X,Y`) and crop the bottom bar |
| `b7-word-outright` | a hit past the HP word; kill-outright on veterans | self-destruct: `click` the unit, then `keys <i> mouse:600,500 ctrl+d` |
| `b7-dgun` | the retail D-gun past the HP word | `order --unit N --expect ARMCOM --expect-target ARMMSTOR blast unit T` |
| `b7-radar-hail` | the radar's owner test under targetable hail | launch with `--los 1 --mapping 0`: mapped with permanent sight, nothing is out of sight |
| `b7-mp-host`, `b7-mp-join` | one meteor, one hit, on two peers | `mp_lobby.sh --map 'WK Hail C'`, then apply one file on each peer |

- **Read HP and kills with `peek`**: `*0x511DE8+0x14357` is the unit array; a unit's slot is
  `engine_index · 0x118` into it, its HP the signed word `+0x108`, its kills the word `+0xB8`. The
  array moves at a load: re-read it.
- **`stance: hold` is a movement stance.** A unit on hold still fires at what comes in range, so a
  victim that must not shoot back is one of the unarmed `VT…` types.
- **The engine prints a kill line only for a unit that has fired** (`+0x110` bit 31) and has kills.
- **Log lines**: `veterancy: …` and `enginefix: …` count their events at powers of two; at load,
  each keyed type logs its level count, each malformed key its reason.

- **`tacli log <i> -g "weapon keys"`** shows each decision's event at its 1st, 2nd, 4th, 8th…
  occurrence, and every keyed weapon as it loads.
- **The unit array**: `peek <i> "*0x511DE8+0x14357:4"` is its base; a unit is `base + idx·0x118`
  with `idx` the roster's `engine_index`. Its order list heads at `+0x5C`, the node's type byte at
  node `+4` (a laser tower stands at 22 and attacks at 8; a hovercraft idles at 41 and attacks at
  6). `+0xFA` is the recently-hit byte, `+0xF5` the last hit's kind, `+0x108` the HP.
- **The cursor**: byte `*0x511DE8+0x2CBE` (1 attack, 3 too far, 15 select, 19 normal); the unit
  under the pointer is `u16 *0x511DE8+0x2CBA`. The roster's `screen=` misses a unit by some 25 px
  at the default zoom, so walk the pointer around it until `+0x2CBA` names the unit; an aircraft is
  picked near its shadow.
- **A submerged enemy is neither drawn nor pickable** without the player's own sonar in range.
- **The AI owns its units' orders**: it replaces an order given to one (type 26) and flies its
  aircraft off. Give orders to the player's units; `attack unit` on the player's own unit
  force-fires, except on the player's own aircraft, which a tower refuses (its order node goes
  back to its standing 46). An aircraft slower than `MaxVelocity=1` leaves the map (the fixture's
  `WKAIR` is 1).
- **`keys <i> pause` toggles**, so send it once; `ctrl+d` starts a selected unit's self-destruct
  and a second press cancels it.
- **A water building spawns on the sea floor** unless the scenario gives it a `height`; a floating
  tower at the sea level (75) is not a surface target for a water weapon.
- **"Under Attack" is read off the screen**: its line is text of colour (195,195,155) at the top
  left of the world view, counted over `tacli shot`s. The notification queue cannot be sampled
  (its consumer takes an entry the frame it is queued), and the per-kind next time
  (`0x50871C`) stays 0 while sound is off.
- **A hit unit's blink** shows in a burst of about eight `tacli shot`s taken while its `+0xFA` is
  above 100: under stock the dot is missing from some of them.
- **A meteor shower centres anywhere on the map**, so a test under one needs a small map; the hail
  maps are Show Down's terrain.
- **Saving and loading in game**: Tab, `ui <i> click SAVEGAME`, `click GAMENAME`,
  `fill GAMENAME <name>`, then `click LOAD` (the save screen's button is named LOAD). Loading is
  Tab, `click LOADGAME`, `select GAMES <name>`, `click LOAD`. **A loaded game starts paused**
  (`keys <i> pause` once), the menu pauses the game while it is open, and **the unit array's base
  moves at a load**: re-read `*0x511DE8+0x14357` before peeking a unit.

### Transported explosions

The same fixture carries C4's types: `TXCOM1` (both `Transported…As` keys), `TXCOME`
(`TransportedExplodeAs` only), `TXNONE` (a name that resolves to no weapon), `TXBIG` (32 000 HP, so
it outlives the cargo loop's 30 000); `TX_BLAST_E` does 111 and `TX_BLAST_S` 222 to anything
within its 2000 area, so a ring's HP names the blast. The towers `TXRL` and `TXRL2` down aircraft,
`TXLLT2` kills a commander in one shot. `--tx-damage N` writes another peer's `TX_BLAST_E`, for the
sync test.

| scenario | what it shows | how it is driven |
|---|---|---|
| `c4-transport-down` | a keyed passenger's carried death beside an unkeyed control | wait until each passenger's `+0x86` names its ATLAS, then `order --unit N --expect ARMATLAS move pos X 1600` over the AI's tower |
| `c4-transport-selfd` | a self-destructing transport, with both keys and with one | select the ATLAS, then `keys <i> mouse:600,500 ctrl+d` |
| `c4-survivor` | a passenger that outlives its transport, then dies on the ground | fly it over the tower as above |
| `c4-kill-all` | kill-all's local branch, single player | `+kill 1` in the chat, with `cheats` set by the file. The AI flies its own ATLAS about and may unload it: read the pair's positions right before the kill |
| `c4-mp-host`, `c4-mp-join` | a carried death drawn on the other peer from the death record alone | `mp_lobby.sh --map 'Show Down'`, one file on each peer, fly the joiner's ATLAS to the host's tower |
| `c4-mp-removal` | a departed player's loaded transports, three peers, one pair in each slot order | apply on one joiner, freeze it, then `REJECT` on a peer. The host's removes the player on the third peer at the same moment, so compare the third's `wire:` `0a` count before and after; after a console `+kill` instead, the third's copies stand until its own `REJECT` |

- **Log lines**: `tacli log <i> -g "transported:"` — the decision, the mark, the kill mark, a
  received carried death, the pick with its weapon, each at powers of two; `-g datakeys` shows each
  key resolved at load.
- **Selecting a carried transport**: the pointer must sit where `u16 *0x511DE8+0x2CBA` names the
  ATLAS **and** the cursor byte `+0x2CBE` reads 15; its passenger is drawn on top of it and reads 19.
- **A carried unit takes no area damage**, and an aircraft is hit well past a weapon's `range` (a
  tower at `range=150` downed an ATLAS 440 away): space the cases thousands apart.
- **A departed player, for real: freeze a peer.** `kill -STOP` its `TotalA.exe` (the pid from `tacli
  ls --json`) and the others raise `TIMEOUT.GUI` within seconds; `ui <i> click REJECT` runs the
  engine's own player removal, and so kill-all for that army. A surrender (`mp_leave.sh`) never
  gets there: it kills its own army before it leaves. `kill -CONT` the frozen one before `tacli
  stop`, and kill its leftover `dplaysvr.exe` by PID after (check the prefix in its environment).
- **A seat's number differs per peer**: each peer is seat 0 on its own screen, and the blocks of
  slots are in DirectPlay-id order. A player's seat on a peer is the record `main+0x1B63 +
  seat·0x14B` whose block start `+0x67` holds that player's units; `+0x73` is 1 for the local
  human, 2 an AI, 3 a remote player.
- **In the chat, a space is the key `space`**: `char:space` types an `s`. `+kill <seat>` needs
  `cheats`, and on a live player it desyncs the game.

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
  store for what they name; `tacli arm` drives them for A/Bs. **Display mode, Monitor and Vsync
  have no lever and are live under `--defaults`**: tacli writes the instance's placement into
  the store on every launch, so a click there lasts one session and the next launch puts the
  instance back on its tile.
- **Under `tagpu_defaults.off` (every launch without `--defaults`) the store has no say and every
  row but Shadows is greyed** — except that the placement (`display`, `window`, `vsync`) is read
  under every launch. Shadows is then the engine's own shadow switch. Test the menu with
  `--defaults`. The engine's Gamma, size and option word come from the instance's registry, which
  a `--defaults` launch saves its store's values into at every game entry — peek them before
  treating a later control launch of that instance as stock.
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
- **Vsync is a row of both screens** — the in-game panel, and the shell's Window column between
  the two sliders. On logs `frame cap: vsync on, a N fps backstop over the monitor's M Hz` (the
  monitor's rate + 1; a present that waits for the blank never meets it; `… over an assumed 60
  Hz` on a monitor whose rate wine cannot read, its secondaries), off `frame cap: none (vsync
  off)`; a click rebuilds the swapchain on the next frame, in place (`swapchain rebuilt in
  place … - the passes kept`), so the restored art stays. A tacli instance's `--vsync` is written
  over it at every launch.
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
  `--res`, `--vsync` (written only when given: the store is the player's copy),
  `--map`, `--player`, `--los`, `--mapping`, `--unit-limit`, `--defaults` and `--sound` work as
  locally. `--window`, `--display`, `--slot`, `--dplay`, `--intro` and `--shipped` are refused.
- **A test folder's registry is a file; TA's settings key on that machine is never written.**
  `tacli-state\registry.txt` holds TA's key, as a local instance's does. Every remote launch
  passes the token `-xtacli-test`, which the engine skips; with it and the `tacli-state` folder
  beside `TotalA.exe`, the DLL answers the registry calls of `TotalA.exe` and `win32.dll` from
  the file, and any other key is read-only; either alone is refused. `launch` puts its values
  (sound off, `Interface Type`, the display and skirmish values) into that file. The log's second line says which mode ran:
  `registry: TEST MODE, entered by the -xtacli-test token and the tacli-state folder beside
  TotalA.exe -- its registry is … hooks: TotalA.exe 9 of 9 registry imports, …`. A test launch whose store is missing or does
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
  is changed through `regstore_update` in `tools/tacli` (`tacli registry` is local only). Nothing writes `tagpu_zoom.txt`
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
