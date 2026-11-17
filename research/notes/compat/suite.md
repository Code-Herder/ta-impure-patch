# The compat suite (tacompat)

## Summary

`tools/compat/tacompat.py` builds every [setup](setups.md), starts it, watches every window the
game opens, reads what each party logs, and compares the outcome with the setup's goal and its
accepted behaviour today. It runs all setups at once on Wine, with a 200-against-200 skirmish
wherever the menu comes up and a two-player network game wherever Impure runs, and one at a time
on the Windows test box. Every run is also read for **any of TADR's code having run**: the goal
of a player's setup is Impure running with none of it. **Nothing may be
UNEXPECTED before a release.** The `ta-compat-check` skill has the whole loop and its traps.

## Running it

- `list` and `fetch`: fixtures with a URL download; the ones behind a browser check are fetched
  in a browser and imported with `fetch --import NAME=PATH`, which checks every member's md5.
  A mod as players have it is what TA Forever installs: its API
  (`api.taforever.com/data/featuredMod`, `installPackage`) lists each mod's packages in order on
  plain URLs, and TA Zero's and TA Twilight's setups layer one fixture per package in that order.
- `wine`: every setup at once. Each gets its own `tacli` instance (`compat-<setup>-<tag>`, the
  run's tag: the lowest slot of the run lock it holds, `tools/runlock.py` TAGS), made with
  `tacli create --private-registry` — registry hives of its own, copied out of the template's
  shared inode before any Wine process runs in the prefix — its own Xvfb display, and the overlay
  of its fixtures on the retail gamedir (the copy is read from the shared inode, which another,
non-private instance's wineserver may be rewriting at that moment: [tacli](../tacli-design.md)
names the gap). The hives are private because the store `tacli` keeps
  TA's key in is off here (a player's launch, below), so a mod's own key (`Software\TotalM`,
  `Software\TA Zero`) and a TADR DLL's writes reach the prefix's registry; and because they are
  private, `--prep-jobs` instances (8) are made at once — 19 in about 50 s. The runner watches the display's windows for the whole run (a refusal is a
  box, and a box is the evidence), reads `tdrawlog.txt`, `tagpu.log`, `startup-failure.txt`,
  `ErrorLog.txt` and the Wine `+loaddll` trace, then fights the battle where the menu came up,
  and last — while the game is still alive — reads its code out of the process (below). Three
  checks hold the claim to what a player sees, not only to Impure being up. **The mod's own
  content**: at the menu, the unit types a setup's `content` names (ones only that mod defines)
  must be in the engine's table of loaded types (`tacli units`, a walk of the table in memory,
  not of any archive path). **The units are seen**: after the battle's scenario, the newest
  `units: alive=N` header in `tagpu.log` — the frame packet, what Impure draws — must hold nine
  in ten of the units the applier made. MEASURED 2026-09-27: Escalation made 401 and Impure saw
  0, which every earlier check had passed. **The battle is played on its own map or not at all**:
  its map (`battle_map`: the `setup.map` of the setup's battle scenario) is saved as the skirmish map under every
  registry root the game may read, as for the ImpAI stage below; before the walk the battle reads
  the root in the game's memory and, when it is another, saves the map there and starts the game
  again (the start-up's verdicts stay the first launch's, and `tagpu.log` holds both launches in
  order); after the walk it reads the loaded map back (`GameingState +0x204`) and fails by name
  when it is not the scenario's. Escalation's battle map was saved under retail's root only, as
  the stage's seats were [INFERRED: its battles played the `TA Esc` key's saved skirmish; the first
  run with the check says]. **The terrain is drawn** (`terrain_drew`): a battle
  fails when the terrain pass refused its tile atlas at any point of the run (`vk: terr: a WxH
  tile atlas is outside what this pass carries - nothing drawn`), or when no `vk: census` line
  after the applier's counts the terrain pass. Once the pass owns the terrain the engine's own is
  skipped, so either is a black viewport under units that still draw, the fault a player reported
  on TA Zero's 6P Wild Hunt. Neither is a sample of one frame: a census is a single frame every
  300, and one frame that drew no terrain is not the fault. The neighbourhood atlas's own refusal
  (`… neighbourhood atlas is outside … - the terrain restores per tile`) still draws and is not
  matched. A run with no census after the applier is not on the Vulkan lane and is not judged by
  it. **The battle's computer seat is ImpAI's** wherever the DLL carries ImpAI:

    - **Why it is ImpAI's.** The battle's skirmish runs from the player's own store, which names no `seatai`. So the
      default every player gets applies: every computer seat is ImpAI's (b8). The gate tests what players run.
    - **What that means for a failure.** A battle UNEXPECTED, or "the battle stood", may be ImpAI's doing. Read the
      battle's `tagpu.log` for ImpAI's lines before blaming the engine or a mod.
    - **What is asserted** (`battle_impai_misses`, from the battle's `tagpu.log`):
        - the level init's `impai: seats` line marks every computer record of the battle scenario (`setup.players`,
          controller `ai`: record 1 in `200v200` and TA Zero's battles). The skirmish is the player's saved one, so the
          level may seat more computer records than the scenario fills; those are marked too and asked nothing
          (MEASURED 2026-11-16, retail on `dll-e04b641a`: `impai: seats 1,3 (impure.cfg) of 4 records`, at the saved
          skirmish's Easy);
        - there is no `could not` line and no content past its tables;
        - once the applier is armed (`scenario: applier armed`), ImpAI's military begins a task for every one of those
          seats (`impai: mil seat= t=`). The anchor is the arming and not `scenario: applied=`: that line is written on
          a render frame after the game-thread tick has placed the units, so the seat's first task can come before it,
          while the arming is written before the state that lets the tick place anything. It is written once a
          process, so a battle log with two applies fails by name.
      A DLL with no ImpAI (no seam line) is asked nothing. A setup whose expectation says
      `"impai": "unavailable"` has no such seat.
    - **Why not the claim.** The stage's and the network game's claim does not fit the battle: the scenario clears the
      start, so the seat has no builder, and the 60-second fight ends at GameTime 1800, before the seat's second pulse.

    Every game runs silent but with a sound device: Wine is kept off
  PulseAudio and its ALSA driver opens a null device (`asound-null.conf`), because TA:ESC with no
  device at all stops at "No sound driver is available for use."; no file of the setup changes.
- **A setup's folder in a test launch** (`as_test_instance`, after `prepare_wine`): the calibration
  lanes (`tools/tasim/engrun.py`, `SetupWorker`) and `defcheck --setup` play tacli's own launches of a
  setup's folder, not a player's. The store `prepare_wine` parks is put back, and the instance is marked
  to run native DirectPlay, as a player's launch here always does: without it Wine's builtin `dplayx.dll`
  stands in for the folder's own, a Patch Loader imported as `DPLAYX.dll` never runs, and the builtin's
  registry imports, which the store does not answer, end the launch at attach (MEASURED 2026-11-13,
  `mayhem-11.3.0`). Its DirectPlay is moved to a port leased for it (`dpport.lease`), saved as the
  instance's `dplay_port`, which tacli patches into the prefix at every launch; the lease is the caller's,
  closed when it removes the instance. So every game launched with native DirectPlay holds a port of its
  own, whatever it plays. In a test launch the store serves the root a mod's exe or loader names as TA's
  key ([settings](../modules/settings.html), *TA's key wherever the exe names it*).
- **No verdict is decided by the wall clock**: `--watch`, `--battle` and `--mp` are SECONDS of
  game time, counted as SECONDS × 30 ticks of GameTime (`main+0x38A47` through `tacli peek`; the
  network game's, the host's) or, at the menu, of presented frames — the UI layer's `GUI flips=`
  heartbeat is logged once every 600 of the render thread's frames (`tagpu_gui_flush`, called
  from `tagpu_overlay_draw`). A wall bound only gives up on a game that stopped: 120 s of
  GameTime or frames standing still, 60 s for a one-frame answer, 300 s for a screen of the menus
  or the battle room to come up. A game Impure is not in has no frames to count: once its main
  window is up its start-up is watched for `--watch` s of the CPU time of TotalA.exe's main
  thread (user and system, `/proc/<pid>/task/<pid>/stat`), which a loaded machine stretches and
  does not shorten, and a CPU time standing still for 120 s ends it too. The main thread's, not
  the process's: Wine's dozens of spinning threads made the process's grow 12.9 s a second
  against the main thread's 0.67 (MEASURED 2026-10-09, gammata-ota at its menu). The whole watch
  gives up after 30 min, deciding nothing. **The watch ends at the first box**, and the run is
  classified on that box and the `ErrorLog.txt` file: a box blocks the game (with Impure's
  refusal up, no thread of it used any CPU for 45 s, MEASURED 2026-10-09 on
  retail+tadr1-recorder), and no fact says that no other box will follow, so a second box is not
  seen. A run whose process exits is read at once, with no wait for a box after it.
- **The network stage** (`wine --mp SECONDS`, 30 by default, 0 for none): for each player's
  setup whose single-player run showed Impure running, a second instance of the same folder
  (`compat-<setup>-<tag>-j`), both games on their own displays, hosted and joined through the game's
  own battle room over Windows' DirectPlay (the walk of `tools/mp_lobby.sh`), each side applying
  its half of a small fight (`scenarios/compat-mp-host.json`, `compat-mp-join.json`), then
  watched on both. Each game is queued as its single-player result comes in, and `--mp-jobs`
  (6 by default) play at once, **each on a DirectPlay port of its own**: the name server binds
  its port for the whole machine, so the suite leases a port a game from 61000 upward
  (`dpport.lease`: one flock a port, so no two runs share one, and above the kernel's ephemeral
  range, so no outgoing connection takes it) and patches it into both peers' prefixes
  (`tools/dpport.py`, the same patch `tacli launch --dplay-port` makes) and the two
  meet there and on no other game's. A holder of a game's port in one of the suite's own
  prefixes is stale and ended; any other is someone else's game, waited for five minutes and
  never touched, and then that game fails as not run. **Every text field of the walk is read
  back before it goes on**, and filled again while it reads anything else: `tacli ui fill`
  reports a short field without failing. MEASURED 2026-09-28, under a full run's load: ProTA's
  joiner's ADDRESS read `1`, its `+winsock` trace shows `inet_addr("1")` and the socket snapshot
  a SYN to `0.0.0.1` — a "no session listed after twelve UPDATEs", the failure that had cost
  about one network game in ten. That is the one failure traced; the earlier ones (Mayhem,
  Escalation, TA Zero) looked the same and were not [INFERRED: the same cause]. With the read
  back, two full runs since played 30 of 30 network games. A joiner that still lists nothing records **every game socket on the
  machine** with its prefix (`mp-sockets.txt`) and keeps both peers' Wine logs;
  `TACOMPAT_WINEDEBUG=+winsock` adds a trace channel to every game, kept whether it fails or
  not. "Its own" is the set of
  prefixes **this run** created, not every `compat-` name: the instance names are fixed, so a
  session in another worktree runs prefixes named exactly like ours
  (`parallel-mp-runs-share-dplay-port`). A setup whose battle room this walk cannot reach a game
  in says so in `no_network_game`, in words and from a run, and plays none: the report's column
  reads `no mp`. `tazero-wildhunt` carries it (its folder's network game is `tazero-alpha5`'s), and
  so does every derived `managed-refuses-*` row. Escalation's two did while its games started with **no
  units on either peer**; the units were there and the sim ran them, but every pass of the DLL
  filtered pointers at 2 GB and ESC's exe is large-address-aware, its heap above that
  ([exe-reverse-engineering](../exe-reverse-engineering.md), *A large-address-aware exe's heap
  is above 2 GB*). Both now play their network game (MEASURED 2026-09-27).
  With `--mp-impai` each peer writes `netai=impai` and adds one computer seat in the battle room
  (the host `PLAYER2` twice, Unused → Blocked → AI; the joiner `PLAYER3` once), and the game is
  also judged by both peers' level-init lines: each marked exactly its own one seat, at level 2,
  and counted the other's as another peer's AI seat, ImpAI's by its `ImpAI:` name; and each peer's own seat
  is held to **the claim**: by its last pulse line (`impai: pulse`, a seat's counts since the level began: one in the level's first 15 ticks, then one a game minute) the seat gave orders (`mroots` + `eroots`) and completed a unit since its first pulse (`built` above the first's, which already counts the commander, MEASURED 2026-11-16); a turn taken alone does not show that the seat played (`impai_claim_misses`). The claim needs a pulse, so the game is played
  past `IMPAI_CLAIM_TICKS` (3900, two game minutes and a margin) whatever `--mp` says. **The fight stays off the
  seats**: no unit either half places can stand within `WAVE_CLEAR` (1500 world pixels) of StartPos1..4, the starts
  the game's four records take (fixed starts put record i at StartPos(i+1)). Every seat is its own team, so a wave
  beside a computer seat's commander is an enemy inside ImpAI's 600-pixel safe distance, and the seat's only builder
  flees: the claim would then measure the scenario. Both halves' waves stand on dry, level ground of Two Continents
  around (7000, 6480), 2681 pixels from the nearest of those starts, and attack each other. `run_wine_mp` checks it
  before each network game against the map in the setup's own files (`mp_wave_misses`; a wave too close fails the
  game by name), and `tools/test_compat_impai.py` checks every setup's halves against the installed map.
  The peers' unit counts are not compared: the
  game is not lockstep — only a unit's owner decides its death and the other peer applies it when
  the message arrives ([network](../engine/network.md), *A computer seat in a network game*) — so
  two counts read at one GameTime differ by whatever is in flight, which depends on load. Nor are
  the deaths compared, though the host's recording carries the joiner's own: each death of a
  joiner's unit is in the joiner's contribution as an owner root (kind 10, `RK_EV_DEATH`, unit and
  birth; `tools/replay/recmp_file.py`). The contributions travel on the recorder's own DirectPlay
  session, the engine's death message `0x0C` on the game's, so nothing orders the two; and the host's
  file closes at its own consumption cut without waiting for the joiner's later contributions
  ([multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.md), *Consumption cuts and
  terminal receipts*). A death the host applied near its end can therefore be missing from the
  contributions in the file, and which one depends on load: the check would not hold by
  construction, so the stage has none. **The gap**: a peer whose world lost a unit its owner still
  has, or the reverse, is not caught here.
  The seats get no alliances, and need none: Start refuses only a game whose every seat is on one
  team, every seat starts on none, and the walk never clicks a team gadget
  ([network](../engine/network.md), *The battle room's Start refuses only a game on one team*).
- **The ImpAI stage** (`wine --impai [SECONDS]`, 900 when given bare; off by default): for each
  player's setup whose single-player run showed Impure running, a third copy of the folder
  (`-a`) plays the bar's seating ([the bar](../impai/bar.md)): record 0 the human with no unit,
  record 1 a CORE computer seat played by ImpAI, record 2 an ARM computer seat OTA AI plays,
  Difficulty Hard, 1000 metal and energy each. **The seats are written where the game reads
  them**, plus `seatai`: the startup's registry load reads them, under the root the game's memory
  names at `0x509EB8` ([process](../engine/process.md), the registry accessor `0x4B6880`). Before
  the launch they go under retail's root, the setup's `registry_roots`, the root the exe's own
  file names there (Escalation's: `TA Esc`) and, for a mod chosen by `mod=`, the one its drop-in's
  exe names (the battle's map is saved the same way); once the menu is up the stage reads the root in the
  game's memory, and when it is another — a loader's rewrite, or the exe image a mod chosen by
  `mod=` lays over the process — seats it too and starts the game again. An accessor whose code
  is not stock fails the stage as `could not run`, by name. After the walk into the skirmish
  (Escalation's front end goes from Skirmish straight into the game: no SKIRMISH screen, the
  saved settings as they are) the stage reads the loaded map (`GameingState +0x204`), the seats
  and their commanders and the engine's Difficulty back, and fails by name on any that is not
  the stage's — never playing another map. MEASURED 2026-11-12 (the survey): with the seats
  written under retail's root only, Escalation and its managed row played the `TA Esc` key's saved
  skirmish, Acid Foursome with two seats. The scenario
  (`scenarios/compat/impai-stage.json`, Town & Country, or the setup's own `scenarios.impai`) is
  played only where the setup's own files hold its map, both `.ota` and `.tnt`, looked up through
  the engine's mount order over the setup's folder (`tools/wiki/tavfs.py`); otherwise the stage
  fails as `could not run: no <map>`, never on another map. It clears the start, places the two commanders apart — each of the type the game itself gave
  that record at the start (its side's commander, which a mod may name otherwise: TA Zero has no
  `CORCOM`), the scenario's own types standing only for them — and
  turns the end test off; after SECONDS of
  game time an end scenario keeps one unit of record 1 and turns it back on, so the level ends and
  its teardown writes ImpAI's level-end lines. **Judged by ImpAI's own lines**: the seat line
  marked record 1 of three seated records, the level end wrote its lines, no `could not` line, no
  refused turn, record 1 took turns, and **the claim**: by its last pulse line (`impai: pulse`, a seat's counts since the level began: one in the level's first 15 ticks, then one a game minute) the seat gave orders (`mroots` + `eroots`) and completed a unit since its first pulse (`built` above the first's, which already counts the commander, MEASURED 2026-11-16); a turn taken alone does not show that the seat played. The stage is played past
  `IMPAI_CLAIM_TICKS` (130 s) whatever SECONDS says; the survey plays none and asks no claim. A seat line that marks nothing is **ImpAI unavailable**, the
  reason the line before it gives (`unavailable: …`, `menus past their tables …`, …); that is a
  failure unless the setup's expectation says `"impai": "unavailable"`. **Content that did not fit is
  a failure whatever the setup accepts**, named by its line: ImpAI's content is all or none
  ([ImpAI in the engine](../modules/impai.md), *The fight content*), so a mod whose `[DAMAGE]` rows,
  weapons or types overflow the fight tables (`impai: fight none: … do not fit the tables …`) or whose
  menus overflow theirs (`impai: menus past their tables …`) has no ImpAI seat, and the stage says
  which table rather than reading the empty mark as an accepted gap (`IMPAI_NOFIT`).
  `tools/impai/netai_check.py` fails the same lines on either peer. The pulse count is also
  reported. The claim's checks are tested offline in `tools/test_compat_impai.py`. `--impai-seat ota`
  plays record 1 with OTA AI as well, which runs the whole harness on two stock seats.
  `--impai-jobs` (6) stages play at once. The report's column reads `ai ok`, `ai FAILED`, `ai n/a`
  (unavailable and accepted) or `-`.
- **The ImpAI survey** (`wine --impai-survey`): the stage with no game time — each setup's
  skirmish seated and loaded, the Difficulty read back and the level init's lines read (the seat
  line, the reason a seat is not ImpAI's, the content lines and any `could not`), nothing applied
  or played: where ImpAI is available, setup by setup.
- **A run that stops can be resumed** (`wine --resume DIR`): the run reads each stage of a setup
  the moment that stage ends, whatever stage of another setup is still playing (`as_each_ends`),
  ends its runs-viewer step then, and writes the setup's results into its folder the moment its
  last stage ends — its evidence files first and its result last as `verdict.json`, written to a
  temporary name, flushed and renamed over — so a setup the run did not finish, in any stage, has
  no verdict file, and every setup that finished has one, whichever setups were still playing.
  A setup is judged again after each stage, in whichever order its network game and ImpAI stage
  end, and the judging keeps nothing of an earlier one. A resume reads the
  verdict files back and plays every other setup whole, after removing whatever a stopped sitting
  left in its folder. `run.json` holds the play arguments and the DLL's hash, and a resume that
  differs in either is refused: the report would otherwise be one run of two builds or two
  suites. What a managed row's own folder showed (a refusal's box, `mod=` left in `impure.cfg`, a
  fetched package's swap) is read when it settles and kept in its result, and the comparison with
  its drop-in row is made again at the report, so the two may come from different sittings.
  What that comparison reads of a row's `tagpu.log` (a loose unit file refused, the name the
  replays give the game) is read when the row is persisted, the last moment its files are in
  memory, and kept in its result as `log_facts`. Both Twilight rows name the game `retail`, as
  their drop-in does, and that is the rule, not a miss: the replays' mod table names a game by
  the first mounted archive one of its rows' patterns matches, and Twilight's row has no pattern
  (its main menu's version label names it, `tagpu_replays.c`), while Twilight 2.0b98 ships one
  archive, `rev31.gp3` (the TA Forever zip holds no other), which no row names; a package's
  `<id>.hpi` is matched only for a row that has a pattern.
- **The verdicts the stage's fixes changed** (MEASURED, Wine, `--mp-impai --impai 120
  --impai-seat ota`): run A `20261112-232644` (DLL `c03a6119`), run B `20261113-012639`
  (`e8a8af21`, DLL `94bb9648`), run C `20261113-023826` (`b9da4344`, DLL `7da3e420`, the sixteen
  setups below alone). Every other setup's verdict was the same in A and B.

  | setup | A | B | C | cause |
  |---|---|---|---|---|
  | `escalation`, `escalation+tadr-dev`, `laa-escalation` | UNEXPECTED, `ai FAILED` | ok | -- / -- / ok (`escalation`) | the stage wrote its seats under retail's registry root, and Escalation's exe reads `TA Esc`, so the game played that key's saved two-seat skirmish: fixed by seating the root the game's memory names and refusing another map (`71a5bd4c`) |
  | `managed-escalation` | UNEXPECTED, `ai FAILED` | UNEXPECTED, the comparison | ok | the same root, then the comparison below |
  | `tazero-alpha5`, `laa-tazero-alpha5` | ok | UNEXPECTED, `ai FAILED` | ok | the stage's scenario named `CORCOM`, which TA Zero has not (its commanders are `ArmCommander` and `GoKCommander`); A's stage applied no scenario on any setup (no `scenario:` line in any of its stage logs), so it never named one. Fixed by placing the commander the game gave each record at the start (`3363684b`) |
  | `managed-tazero`, `laa-managed-tazero` | ok | UNEXPECTED, `ai FAILED` and the comparison | ok | both causes |
  | `managed-mayhem`, `managed-prota`, `managed-twilight`, `managed-fetched-prota`, `laa-managed-mayhem`, `laa-managed-prota`, `laa-managed-twilight` | ok | UNEXPECTED, the comparison | ok | the managed row's comparison with its drop-in read `tagpu.log` out of results whose files the resume's persist had dropped, so every managed row read "no loose unit file refused" and "the replays name this game None": fixed by keeping what the comparison reads as `log_facts` (`3363684b`) |

  In C every managed row's `log_facts` holds a refused loose unit file and the drop-in's game name,
  its CRCs equal its drop-in's, and all sixteen played `mp ok` and `ai ok`.
  Windows runs one setup at a time on the owner's desktop and has no resume.
- `windows`: one setup at a time on the Windows test box, over SSH, with the game and a window
  watcher (`win-watch.ps1`) started as scheduled tasks in the logged-on session, because a
  process started over SSH cannot see the desktop's windows. Every setup runs from a work folder
  at a **path Windows has not started the exe from before**: the first start from a new path is
  the one Windows' compatibility engine hooks the exe's `DirectDrawCreate` import on, and a folder
  reused across setups tests only later starts — v0.3 refused every fresh Total Mayhem install
  while this runner, with one folder, passed ([the takeover](takeover.md), part 1).
- `selftest`: the hook decode against every shape it claims to decode and every byte pattern it
  must not judge on, with no game and no fixtures — eleven cases, each built from something that
  was measured: the Patch Loader's own thunk rewrite, a plain immediate, the two coincidences that
  refused a launch on the Windows box, and a run holding a call into the mod's own DLL *and* a jump
  into TADR, where the TADR one has to win. It is the only check of `decode_run` that does not need
  a running game, and both platforms' verdicts rest on it.

A run is **meets goal**, **known gap** (matches `today`) or **UNEXPECTED**; the exit status is
1 on any UNEXPECTED. The rule: no UNEXPECTED before a release.

**Every player gets a commander, in both games.** Before any scenario touches the map, the
skirmish asks the engine (`tacli units`, the catalogue's `players` and `commanders`,
`tagpu_cat.c`) for two lists: every **seat in the game** — a player record whose first dword is
nonzero and whose controller byte `+0x73` is 1 to 4 (local or remote, human or AI) — and every living unit whose type has the FBI `commander` bit
(`UnitDef+0x245` bit `0x40000`, [engine map](../exe-reverse-engineering.md)). Every seat must
hold a commander, and there must be two seats at least. The seats come from the player records
and not from the units, because a player the game gave nothing owns no unit to be counted by.
The network game checks the same on **each peer**, so a peer that never received the other
player's commander fails too. It is not read from the roster: that is what Impure draws, and the
other player's commander starts under the fog. The catalogue answers only while a level is in
play (`in_play`; before that the loader thread may be building the unit array), and the suite
polls until it does. The battle scenario clears the map (`clear_existing`), which is why the
check comes first.
MEASURED 2026-09-28: retail's saved skirmish has three players (ARMCOM, and two CORCOM for the
AIs, seats 0, 1 and 3); TA Zero's starts a GoK and an Arm commander; each network peer holds both,
TA Zero's included.

**A mod without the stock unit types names its own scenarios.** The battle (`200v200`) and the
network game's two halves (`compat-mp-host`, `compat-mp-join`) place stock types, which every mod
the suite runs keeps except TA Zero, whose factions are redesigned from scratch (no `ARMPW`, no
`ARMCOM`). A setup's `scenarios` names its own for the three roles — `tazero-alpha5` uses
`compat-battle-tazero` and `compat-mp-host-tazero`/`-join-tazero`, the same fights in TA Zero's
types. **The skirmish's map is the battle scenario's** (`setup.map`, read by `battle_map`), written
as the saved `SkirmishMap` into every registry root before the game starts: Two Continents for
every scenario but `compat-battle-tazero-wildhunt`, `tazero-wildhunt`'s, which moves TA Zero's
fight onto 6P Wild Hunt's central plateau. The network game's map is `BATTLE_MAP`, Two Continents,
for every setup. The walk knows the game screen by its in-game panel, `<side>MAIN.GUI`, for any side: TA
Zero's third is GoK, and a hard-coded `ARMMAIN|CORMAIN` never saw a GoK player's game start.

**The UI layer is part of every verdict.** A run whose Vulkan UI layer gave up following the game
(it then composites nothing: a black screen) or asked for a fresh start past the start-up is
UNEXPECTED, read from the layer's own log lines. Every other check here reads state and gadgets, so
before this a black screen passed: every Escalation run gave up on 2026-09-27 and no row said so.
The `gui-stress` setup drives the race on purpose, a fresh start every 120 presents while colour is
valid, at the menu only (a setup with a lever plays no battle and no network game: run on through a
battle, the stress spends the layer's 32 repaints of a palette generation by design), and passes
only if the layer never asks for one of its own and its fallback is seen to run
([GPU status](../gpu-status.md), *Two colour disagreements are neither*).

**A device that refused the renderer its memory is UNEXPECTED, on every peer.** A pass losing its
memory is what a player would see, and no pixel check sees most of them: a dead HUD leaves the side
panel right. So every refusal is judged by name from `tagpu.log`, in the single-player run and on
both peers of the network game (`DEVICE_REFUSED`). The first full run to meet them scored `twilight`
and `loader+tadr-mayhem` "ok" with their HUD gone. **A refusal of the GPU memory cap fails a game
whatever its setup expects** (*CAP REFUSED*): a lever that refuses the device on purpose
(`refuse_lever`, `stage_refused`) excuses the device's refusals, never the cap's, which mean the
suite's figure in `tools/runlock-suites.cfg` is too small. The run (both its launches), each peer of
the network game, the ImpAI stage and a mod's install start are each read for them across every
launch and rotation once the game has stopped (`talog.CapWatch`).

**The suite cannot refuse itself its memory (`GameSlots`).** The pools are sized for the CPU, and
`--jobs` single-player games plus `--mp-jobs` two-player games is 18 at about 900 MiB each, which filled
a 12 GB card. A run takes one slot before its game starts and a network game takes two, all at once, up to
the run lock's `games()`: the games its share of the GPU budget holds (`tools/runlock.py`, THE GPU
MEMORY), taken in a queue of their own before its CPU slots, so every run on the machine together stays
inside the device's memory less a reserve (the lock's `runslots/vram.cfg`, seeded from
`tools/runlock-vram.cfg`). A game takes its cap plus the driver's allowance and a margin of the budget, and
runs under that cap by construction: `start_game`, through which every game of the suite starts, writes the
run's cap into the game's store (`gpumem=`, `runlock.write_gpumem`) and starts the game under a keeper that
holds a seat of the run until every process of the game is reaped (`runlock.start_game`), so the suite never
has more games on the GPU than its `games()`
([the Vulkan backend](../modules/vulkan-backend.html), *The GPU memory cap*). A network game's two
peers wait for each other in the battle room, so they need their two seats at once: a run granted
fewer than two games plays no network game (`runlock.together`), and each setup's reads `could not
run` by name, since its joiner's launch would wait for the seat the host holds while the host waits
for it; `GameSlots` refuses more slots than the run has. The suite's cap is
`compat` in `tools/runlock-suites.cfg`: a provisional 1536, 5 games on the reference setup, until
`vramproof.py watch --cap N` measures the suite. `games at once:` in the output says how many it got. Where no
device memory reads (no `nvidia-smi`, no amdgpu counters) there is no GPU budget and the pools alone bound
it. A game launched by hand outside the run lock is not counted; the refusal it causes is named above.

**Where a network game's time goes (a gap).** The network game itself is short: `play_mp` takes
78 to 128 s a setup (MEASURED 2026-11-13 in the verdict files of two full runs, 31 and 16 setups),
of which 900 ticks at speed 10 are 30 s. The rest is split [INFERRED, from four setups' host
`tagpu.log` start against their recording's start time, no per-phase timestamps]: 39 to 46 s from the
host's launch to the game starting (both peers launched, their menus, the battle room walked, Start),
then the two scenario applies and the 900 ticks, then 10 s (retail) to some 40 s (Escalation) of hook
reads, type CRCs and the two stops; what makes Escalation's tail four times retail's is not
established. Speed 20 would halve the 30 s, but a network game cannot turbo and plays variable
batches at speed 20, so the suite keeps speed 10: that is the owner's call, not taken.

**The `pass-refused` and `pass-refused-gdi` rows** refuse the UI pass in a live battle by
`refuse_in_battle`, which writes `tagpu_passrefuse.on` during the battle's side-panel step, since a setup
with a lever plays no battle. `once`: the panel is opened, the lane is refused and has to come back and
present, and only then is the panel closed, so its restore meets the emptied save-under every time, and
the side panel has to pass. `keep`: the session has to reach GDI, and GDI's screen has to come to show
the engine's side panel within 60 s. `pass-refused` also opens `HELP.GUI` from the in-game menu across a
second refusal (`dialog_after_reset`): the view has to change when it opens and be back when it closes.
`pass-full` and `pass-full-gdi` are the same two roads with the
driver refusing every allocation of the rebuild (`tagpu_vkfull.on`), and `wiki-stage-refused` refuses the
wiki's stage alone: the page loses its 3D image, and the lane has to stay up.

**On Windows** the refusal rows run only under `--smoke`, whose battle is where the refusal happens
(`refuse_lane`): the log has to say the lane came back, or that the session went to GDI, and the game has
to stay alive through the watch. On GDI the screen's side panel has to be the engine's (`win-shot.ps1`, the window
letterboxed back to the engine's size, against a shot from before the lever): 0.003 % of the strip differs, MEASURED
2026-11-07. A Vulkan picture is not judged there: on that setup's AMD card a copy from the screen and `PrintWindow`
read black, and desktop duplication gets no frame. `pass-card-full` is the card filled for real (`vkfill`), Windows only, as the Wine
path's GPU is the desktop's as well. `wiki-stage-refused` is left out: that path has no way to open the page. A setup with a lever
plays no smoke, by the rule above.

**Test mode's attach line is judged on every Windows smoke row** (`store_misses`): a folder DLL started before Impure
fails it, but for the last one named on a route where TADR's `DllMain` loads Impure (`tadr_started`), and a row's `store_covers` names DLLs no import table reaches, such as the forwarder's `tplayx`. `store_loaded`
has the row open the Multiplayer screen first, so that `online.dll`'s extension DLLs load during play and answer.
`store_lookups` names where code that looks up registry functions through `GetProcAddress` runs, each of
which must log one answered by the store: `in no module` on Total Mayhem and ProTA, where `basscd.dll`'s
unpacked code runs.

**Two checks read the UI's pixels, or what stood in for them.** The producer's `GUI pixels:` line
counts every box that crossed as engine pixels — which the layer drops — by cause, and a
`copy-freed` there is UNEXPECTED: a copy whose source surface was freed before it was published,
the class of Escalation's stale orders panel ([GUI renderer](../gui-renderer.md) §28). The
battle stage, before its scenario, selects the commander, deselects it and compares the side
panel below the minimap on screen with the engine's own surface, once a shot taken after the input
and two UI heartbeats (at least 600 presented frames) say the screen holds the engine's answer:
over 5 % different is UNEXPECTED,
and a deselect that did not change the engine's panel fails as not exercised. Measured: v0.3 22.5 %
on Escalation, the fix 0.12 %, retail 0.06 % on both.

## Whether TADR ran

`tadr_ran` in a setup's goal is judged two ways, on every peer of every stage: from what TADR
leaves in the game folder, and — since 2026-09-27 — from **reading the running game's own code**.
The second is what the first cannot give.

**From the game folder.**

- **`tdraw.dll`** writes `tdrawlog.txt` from its `DllMain`: the file at all means it started
  (the result says too whether it installed its engine patches). **On the routes where TADR's own
  `DllMain` is what loads Impure** — the 3.9.02 exe, Escalation, TA Zero, TA Twilight — it has
  written that first line before Impure exists, so the file cannot be absent there however
  completely the takeover stops the rest. Those setups carry `tadr_started` in their goal, which
  allows that one line and nothing else — `<n> --- Process Attached`, followed by `.  config=<name>`
  in the builds from 2026 and by nothing in TA Zero's 2025 build: an engine patch installed, a recorder log, or any site of the game's code leading
  into TADR still fails them.
- **The recorder** (`tplayx.dll`, or the 2006 `dplayx.dll`) writes `log\TA Demo Recorder Log
  -<date>.txt` **only when it starts from inside one of its DirectPlay exports**. Any such log
  written during the run means it ran; the absence of one does **not** mean it did not, because
  its other way in — the jump it splices over the exe's entry point — writes no log at all
  ([the takeover](takeover.md), part 1). A demo file is no evidence either way: the 2006
  recorder wrote none in a network game while it ran.

**From the running process** (`exe_hooks`). The exe's executable sections are read out of the live
game and compared with `TotalA.exe` on disk; every run of changed bytes is decoded for the address
it leads to (`decode_run`: `E8`/`E9` rel32, `FF 15`/`FF 25` through a pointer, `push imm32; ret`,
`mov eax,imm32; jmp eax`), and that address is tested against every module loaded from the game
folder. **A verdict rests on an instruction that transfers control, never on four bytes that
merely hold an address** -- those are reported as held and not counted, because the bytes are as
likely to be the middle of an instruction or the displacement of a jump ([the takeover](takeover.md),
part 3, where both halves of that are measured). A target in a module whose file carries
`TADemo-MKChat` is TADR's code having run; a target in any other DLL of the folder is a byte the
mod itself sets and is reported, never counted — the Patch Loader rewrites three of the exe's
import thunks into direct calls to the mod's `win32.dll`. One finding a run, and **a TADR target
wins the run**: the bytes before a changed run can be a stock `FF 15` through an import slot that
leads into the mod's own `WIN32.dll`, and taking that one would hide a hook behind it.

**The exe's import slots are read too, on Wine.** The layout comes from the exe file (`pe_imports`:
each descriptor's first slot and how many), the bound values from the process, and a slot leading
into a TADR module counts exactly as a site of the code does — it is the other way a call leaves
the image, and the code that reaches it is stock, so the comparison of the code cannot see it. A
slot bound into the very DLL its descriptor names is **not a change at all** and is left out: the
retail exe's own imports of `WIN32.dll` and `smackw32.DLL` are sixteen such slots on every setup,
and reporting them would bury the one that matters. The exception is a descriptor whose own DLL is
TADR's, which is precisely the recorder installed as the game folder's `dplayx.dll`. The Windows
watcher does not read the slots at all; a row whose slots were not read says so.

The decode is deliberately the same set of shapes `tagpu_takeover.c` decodes in-process, down to
the five bytes of lookback before a changed run, so the suite and the DLL answer the same question
and may be compared. One divergence is left and is deliberate: the DLL reads an `FF 15`/`FF 25`
pointer only where the read is safe by construction — inside the exe's image or inside a
game-folder module — while the suite, reading from outside, reads any address.

**A goal of `tadr_ran: false` is not met by a comparison that did not happen.** When the read
fails — no process to read, the exe mapped away from its `ImageBase`, `ptrace_scope` refusing — or
when no read was attempted at all,
the row says so and reads UNEXPECTED. It is not counted as TADR having run: it is counted as
unproven, which is the same rule as *a run is only evidence if the watcher saw it*.

- **On Wine** the read is `/proc/<pid>/mem` of the game. Only descendants of the pid the runner
  started are looked at (`wine TotalA.exe` maps the PE in that process or in a child of it, and a
  `ptrace_scope` of 1 lets a process read its own descendants and nothing else, so another
  session's game can never be read). A module's extent is the `SizeOfImage` in its PE header read
  out of the process, because the maps show a PE image's header page alone. A module of the game
  folder is found by **where its file really is**, not by the folder's path being a prefix: tacli
  links the retail install's own files into an instance instead of copying them.
- **On Windows** `win-watch.ps1` does the reading — `ReadProcessMemory` from the 32-bit
  PowerShell, since a 64-bit one lists only the exe among a 32-bit game's modules — and **judges
  nothing**: it emits the changed runs with a little context each side, and the module table with
  each module's extent and whether its file carries the marker, for the same `decode_run` to
  decode. Its one gap is the indirect call form, which needs a read at an arbitrary address the
  watcher does not make. It has run over all sixteen setups (2026-09-27), and the rule above is
  what it bought: Wine and Windows map a DLL to different addresses, so a coincidence that fires
  on one machine fires on nothing else, and only a run on the other platform shows it.

`retail+tadr1-recorder` is the check of both kinds of evidence at once: the 2006 recorder with the
takeover switched off (`tagpu_takeover.off`), whose goal is `tadr_ran: true`. MEASURED 2026-09-27:
its log appears *and* the process read finds **24 sites** of the game's code leading into the
recorder's module on Wine, where a battle and a network game let its DirectPlay path run as well,
and the 6 the entry point alone installs on the Windows box, which stops at the main menu — against
0 on every setup where the takeover is on.

## Adding a setup

Every third-party file is a fixture in `tools/compat/fixtures.json`, named by md5; nothing
third-party is in the repository. A setup in `tools/compat/setups.json` lists the fixtures it
overlays on the retail folder, its `goal`, and, while it falls short, its `today` — written from
a run, not from expectation. The commit that makes a setup meet its goal deletes its `today`.
A Patch Loader that moves the registry (`RegistryPath=`) needs the setup's `registry_roots`, or
the battle stage opens on a map the mod does not ship. `calib` gives what a calibration lane on the setup needs that its
files do not say: `unit_limit`, the cap its own ini sets (Escalation's 1000), and `end_type`, the lane end's
unit where retail's `CORFUS` is missing (TA Zero's `ARMT1SOLAR`); a lane reads both back
([the calibration runner](../modules/tasim-calibration.html), *On a compat setup*). `levers` are empty files the setup puts in
the game folder — a DLL switch. A setup with one checks a mechanism rather than a player's
folder, and `content` names three unit types only the mod defines (see above): every setup that
ships a mod's own content carries one. `mayhem-11.3.0-net` switches the takeover off (`tagpu_takeover.off`) so that the safety
net has something to catch, and its goal is the net's refusal. `no_network_game` holds a setup out
of the network stage with its reason, measured (above); none does today.
