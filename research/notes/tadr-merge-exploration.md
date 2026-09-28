# TADR merge exploration

Source read: `github.com/tanvanman/TADR`, local clone (2026-09-07); line counts measured from the
tree, not the release binaries. Scope: what the DLL TA: Escalation ships actually patches into the
game, how large the two halves of the TADR repo really are, and the options for folding that work
into the tagpu stack without adopting its renderer or recorder wholesale.
`[VERIFIED]` = read in the source; `[BIN]` = checked in the shipped binaries (their strings, the
Delphi unit list, or disassembly); `[CLAIMED]` = asserted in in-repo prose, not re-verified here;
`[INFERRED]` = reasoned, not measured.

## The patch stack and the boundary

TA: Escalation authors no engine patch. It ships the community stack renamed. The game-patch DLL —
`TAESC.dll`, byte-identical to upstream `tdraw-escalation.zip` v2026.8.6 — is the subject; its
neighbours are out of scope per the brief. [VERIFIED]

| Binary ESC ships | What it really is | In scope? |
|---|---|---|
| `TAESC.dll` | `tdraw.dll` (TADR, escalation build) — the game patch | yes — the subject |
| `eplayx.dll` | `tplayx.dll` (TA Demo Recorder, Delphi) | excluded — but see "a second engine patch" below |
| `ddraw.dll` | `cnc-ddraw` (FunkyFr3sh) — the DirectDraw re-implementation | excluded |
| `TotalA.exe` | hex-edited: import renames + data-path renames + 3.9.02 patches | excluded |
| `emusi.dll` | "TA Music Player" — CD→MP3 shim | out of scope |

The byte-level proof of the rename is in [deep-ta-esc.md](deep-ta-esc.md); the lineage is in
[project-map.md](project-map.md).

## tdraw.dll — the game patch

The C++ half (`src/DDraw/`) is ~40 feature modules in six groups. Every limit, address and flag
below is quoted from `tdraw.txt` / `config_escalation.h` / the module source. [VERIFIED]

### A. Raised ceilings

**Ported: every limit below is raised in our stack** (2026-09-24). *Ours* is the value we ship
and *Landing* where it landed: L1–L7 in [the section-A plan](tadr-port/raised-limits.md), A′2 and
A′3 in [A′. Content IDs](tadr-port/content-ids.md). Two values differ from TADR's on purpose, in
bold.

| Limit | Stock | Patch | Ours | Landing |
|---|---|---|---|---|
| active projectiles | 300 | 3000 | 3000 | L1 |
| explosion effects | 300 | 3000 | 3000 | L1 |
| flying model-piece slots | 100 | 1000 | 1000 | L1 |
| aux debris / effect records | 300 | 3000 | 3000 | L1 |
| units per player | 250 | 1500 (ESC ships 1000) | 1500, the default and the ceiling | L2 |
| pathfinding cycles | 1333 | 66650 | 66 650 | L2 |
| special-effects vector | 400 | 20480 | 20 480 a layer | L3 |
| unit-type IDs | 512 | 16000 | **16 384**: the type masks are 2 KB, one bit a type | A′2 |
| weapon IDs | 256 | 4096 (ESC on) | 4096 | A′3 |
| model composite buffer | 600² | 1280² | 1280², bounded by area and grown up to 2048² | L4, L7 |
| simultaneous sounds | 8 | 128+ | **32**: the engine tracks 32 playing sounds, and a 33rd plays untracked | L4 |

We also raise one limit TADR leaves at stock, the wreck pool (2048 → 8192, L6), with two fixes to
how a reclaim pays: [Fixed beyond TADR](tadr-port/raised-limits.md#fixed-beyond-tadr). What stays
open is in the plan's [open questions](tadr-port/raised-limits.md#open-questions).

### B. Simulation bug fixes

TADR's list reads as ~15 fixes. Checked against the retail exe (2026-09-25,
[the evidence pass](tadr-port/sim-fixes-evidence.md)), each item is one of four kinds, and **only
the first is ported as B** ([the plan](tadr-port/sim-fixes.md)):

- **Stock defect**: a defect in the 3.1 engine's own code, in any subsystem.
- **TADR's own feature**: a fix to code only TADR has (rotation, its logger, its map spawns).
- **Gameplay change**: a rule change presented as a fix.
- **Diagnostics**: crash reporting, observe-only guards, and code that is dead or a no-op on 3.1.

The items below are the ones this section's summary named; the evidence page covers the rest of
`TABugFix.cpp` and the recorder's plugins.

#### Stock defects

- **"Units exploding in factories"** (`FixFactoryExplosions`). A damage message names its victim by
  slot, and the owner reuses a freed slot at once, so a late hit kills the new nanoframe. TADR holds
  freed slots 5 s, which is timing. **Ported, redesigned**: an incarnation on the wire and a
  two-tick hold (B4).
- **The ghost commander** (`GhostComFix`). A remote unit whose first update arrives before it exists
  is created at (0,0) and corrected only after N ticks. **Ported, redesigned**: the cause measured,
  then an ordering (B5). TADR's Assist would morph the commander and is not ported.
- **Area-damage overflow** (`AreaDamageOverflow`). Two defects: past 20 units (and past 64
  features, which TADR misses) a victim is hit once per footprint cell; and aircraft stacked on one
  cell take no splash. **Ported, redesigned** (B1, B2).
- **Resurrection wireframes.** The failure branch is stock, but no stock trigger was found. **Measured,
  else parked.** TADR's fix writes into grid cells it has not checked.
- **Ctrl-Z/A/B/C truncating IDs ≥ 512.** **Already done** by A′2's 2 KB masks.
- **The crash fixes that are stock**: flak's divide by zero (TADR only logs it) and the stockpile HUD
  divide. **Ported** (B1, B6).
- **Wind**, per peer in stock. **Ported, redesigned**: one wind a game (B6).
- **Two stock defects TADR carries but its summary does not name**: a yardmap read past its string
  (TADR's NULL would crash six readers; **ported, redesigned**, B6), and the download-menu blocks
  left uninitialised (**already done** by A′2).

#### TADR's own features

- **The rotated-unit crash fixes** (staircase yardmap, the builder inside a rotated footprint, the
  return stack) and **the resurrection fix's rotation half**. We have no unit rotation.
- **The long-path and print-screen crashes**: TADR's own logger and surface wrapper.
- **The spawned-command crash**: TADR's own map-spawn feature passed the wrong context.

#### Gameplay changes

- **The grid-claim tie-break.** Written for lockstep; under state replication the firer's peer
  decides every hit, and the change alters single-player behaviour. Not ported.
- **The four escalation rules**: the repair rate (stock heals a flat 1 HP a call per repairer, the
  game's rule), the share guard (group E), aircraft wrecks falling (no stock flyer leaves a wreck;
  the cargo path is measured), and off-map anti-air. Under the off-map margin sit two real edge
  defects, an off-by-one and a line-of-sight shear, which are **ported** (B1); the margin is not.

#### Diagnostics, and items that did not hold

- **Black screen on join**: a "potential" fix with no mechanism found. Parked.
- **Zero-shade faces**: a real draw defect, moot on Vulkan; the GDI lane stays stock.
- **The cargo-detach "Option A/B"**: not a defect, since the owner replicates the detach; porting it
  would add one.
- The order-dispatch guard, the crash rings, the player-lost guard: observe-only, dead, or a no-op.

### C. New data keys

Weapon TDF flags (`nottoair`, `nottounderwater`, `notoverwater`/`notoverland`, `surfacefire`,
`nomapweaponalert`, `reloadbar`); unit FBI keys (`VeterancyThresholds`, `VeterancyAccuracyBuffRate`,
`TransportedExplodeAs`/`TransportedSelfDestructAs`, `Rotations=`, `PreviewPieces=`/`PreviewObject3D=`/
`PreviewFaceOpponent=`); weapon `ID=` 0–4095. The mechanism is the portable one: hook the engine's
own TDF reader (`0x4C46C0`/`0x4c4760`/`0x4C48c0`) — never write a parser.

**Planned 2026-09-25** in [C. New data keys](tadr-port/data-keys.md), after
[an evidence pass](tadr-port/data-keys-evidence.md): four landings, TADR's key names and documented
meanings. `ID=` landed with [A′3](tadr-port/content-ids.md). `Rotations=` and `reloadbar` go to
group D with the features they switch on.

### D. UI / quality-of-life

Megamap, nanoframe ghost preview, building rotation, Mex/WreckSnap, drag-queued orders, con-unit
behaviour (hold-position = reclaim-only; stay-put after build; auto-kickout), weather report, visible
map DTs, unit status counters, team-coloured nanolathe, reload bars, accessible chat, unicode, TA
Hook + whiteboard, HUD polish.

### E. Multiplayer / anti-abuse

Start positions by team, `+autoteam`/`+randomteam`, vote-to-reject, share guard, `.take`
arbitration, lag-switch guard, map unit spawns, CRC reports, challenge-response anticheat, lobby
conveniences (start with 1 player + AI, `.noshake`/`.ready`/`.autopause`, 10-player replay).

### The escalation delta

Escalation = mainline ("prota") **minus mex snap**, **plus** share guard, repair-rate fix, air-wreck
fall, 32-tile off-map AA, and extended weapon IDs. Six knobs differ; the other ~30 are identical.
[VERIFIED]

## tplayx.dll — the recorder

The brief set the recorder aside, but it is a second engine patch, and one thing real content relies
on exists only there: the COB getters. Two facts decide what is worth porting. The first is which
build players actually run; the second is what that build does.

### What ships is not what the source holds

`src/Recorder/` at `dcff5dd` is a 2015 trunk, `4.0.0.538` in `tplayx.dof`, and **no build of it was
ever released**. The recorders people run are older and much smaller. Delphi writes the name of every
unit it compiles into a `PACKAGEINFO` resource, so a binary's plugin set can be read off the binary.
The plugins' description strings and patch-site immediates then show which of those units are
actually linked in. The COB handler was disassembled in each build. [BIN]

| Build | Where it comes from | Its source | Plugins linked in |
|---|---|---|---|
| **3.9.2.416** | ESC GOLD 10.2.0's `eplayx.dll` | Not in the repo. Its COB handler is `bf1172c` (2014-03-03) with all but eight cases removed, and its unit limit is the hard-coded 1500 that `5e08c76` (2014-05-15) replaced. So it was built from a tree dated between those two commits | error log, thread marshaller, input hook, speed lock, pause lock (which writes nothing), unit limit, multi-AI, line-of-sight sharing (seven units), COB getters |
| **2026.9.9-esc** | TADR's `src/Recorder/dist/escalation/eplayx.dll`, in `tdraw-escalation.zip` since 2026.8.9 | Not in the repo. Its version comment reads "Feature set of the shipped 3.9.2.416 recorder, rebuilt from source". That holds for the plugins but not for COB: its getters follow `5e08c76`'s layout, about fifty ids | The 416 set plus `Colors` (its mid-2014 form). `KeyboardHook` and `AimPrimary` are named in the unit list, but their code is not linked. It adds a wire cipher and demo encryption (`WirePacketCipher`, `ChaCha20`, `X25519`, `RsaKem`, ini key `WireKeyAgree`), and their source is absent |
| **4.0.0.538** | the tree at `dcff5dd` | the tree | about forty; see [the 4.0 prerelease](#the-40-prerelease-never-shipped) |

Three consequences for the port:

- **The contract is the 416 set.** Content written for "the recorder" was written against what players
  run. ESC's scripts use exactly the eight getters 416 answers, and no 4.0 data key, script callin or
  order.
- **The 2026 dist is a hazard, not a reference.** It numbers its getters the 2014 way. There, `get 101`
  creates a unit, `102` kills one and `103` spawns a ring of units. HEAD calls the same ids `UNITZ`,
  `UNITY` and `TURNX`, and 416 returns 0 for them. The dist's state-changing ids check only that no demo
  is playing, not who owns the unit. [BIN]
- **The source tree's size overstates the recorder.** For scale, the tree:

| Component | Files | Lines |
|---|---|---|
| `src/Recorder/` total | 89 | 67,333 |
| − third-party (Synopse mORMot) | 2 | −32,257 |
| recorder-authored ≈ | ~87 | ~35,000 |
| └ plugins/ (engine-patch layer) | 56 | 16,212 |
| └ TAMem/ (the TA memory map) | ~8 | ~6,400 |
| └ network (DirectPlay + lobby + packets) | ~10 | ~10,000 |
| └ commands / replay / misc | ~13 | ~3,000 |
| `src/Server/` (replayer, Server.exe) | 20 | 13,092 |

`src/DDraw/` (`tdraw.dll`) is 219 files / 66,752 lines, within 1% of the recorder tree. Most of the
recorder's plugin layer, though, is the 4.0 prerelease. [VERIFIED]

### The shipped features, by port group

Each item names its class. **sim** changes state that every peer shares, **wire** is the protocol
between peers, **local** is one peer's UI or diagnostics, and **tooling** is recording and replay.
The letters are [the port's groups](tadr-port/overview.md#the-groups); F is new, for the COB getters.

**How the recorder acts.** It proxies `dplayx.dll`, so it sits between TA and DirectPlay and can drop,
inject or rewrite any packet. Most features below work that way rather than by patching code. Most
chat commands run on every recorder that sees the line; "host" means that only the host's line counts.

#### A. Limits

| Feature | What it does | Class | tdraw | Ours |
|---|---|---|---|---|
| Unit limit (`UnitLimit`) | Writes 2 bytes into each operand at `0x491640` (the default for TA's own ini read), `0x491659` and `0x491666` (the ceiling). 416 hard-codes 1500; the 2026 dist reads the ini. It writes after tdraw's `DllMain`, so under ESC its 1500 is the ceiling whatever `TAESC.ini` says [INFERRED from the write order] | sim | `LimitCrack.cpp`, 4-byte writes | ported, L2 |
| Several AIs in a multiplayer game (`MultiAILimit`) | NOPs the one-AI check at `0x447D87` in the battleroom's Multi handler, and replaces the AI name format at `0x45130F` (16 characters, as stock) | wire (lobby) | none | none. Group E; first test that several AIs run by one peer share no engine state |

#### F. COB extensions

Recorder-only, and the one recorder feature real content depends on. See
[F. The COB getters](#f-the-cob-getters) below.

#### Line of sight shared among allies: a decision for the owner

Once the recorder loads, allies share sight, radar and unit panels **unconditionally**. `.sharelos`
sets a flag (`TPlayerData.SharingLos`) that nothing reads. TA's own per-player share bits (PlayerInfo
`+0x97`: ShareLOS `0x08`, ShareRadar `0x40`) are ignored. [VERIFIED, BIN]

| Unit | Change | Class |
|---|---|---|
| `LOS_UpdateTables` | Every sight stamp (`0x482270`) and removal (`0x481D50`) is repeated into the grid of each player the owner allies | **sim**: acquisition `0x40AA40` reads that grid through `0x465AC0`, so units target what only an ally sees |
| `LOS_PlayerSeeUnit` | `UnitInPlayerLOS 0x465AC0` answers "visible" for any allied owner **before** its cloak and submerged tests. The HotUnits cull (`0x48BC3D`) loses its own-player shortcut | **sim** (acquisition; the order code `0x439761`), and draw |
| `LOS_Radar` | Re-runs the radar passes of `0x467440` once per ally, writing that ally into `main+0x2A43` while they run | **sim**: unit `+0x110` bit 8 feeds every player's radar list |
| `LOS_MiniMapUnits`, `LOS_GUIText` | Allied dots on the minimap (`0x466E6F`). Allied units' resources, health and kills in the unit panel (`0x46AC85`, `0x46B013`, `0x46B10E`) | local |
| `LOS_AllyPlayer`, `LOS_extensions` | After an alliance change (`0x452B54`, `0x452B5E`) or a `+view` switch (`0x416BBB`), run the full rebuild `0x4816A0` on the game thread (`0x46555F`), so stamps and removals keep balancing | sim |

TA replicates per owner rather than in lockstep, so a peer without the recorder does not desync; it
just sees less. **Not to be ported as TADR has it**, because it reveals information and changes
targeting. If it is wanted, it becomes a group-E rule gated on TA's own share bits or on a lobby
option. It must pass the player explicitly instead of rewriting `main+0x2A43`, and it must be tested
against our fog replica and our line-of-sight shear fixes: two of those, `0x46778A` and `0x4677D0`,
sit in the radar pass TADR re-runs.

#### E. Game control, unit transfer, anti-cheat

| Feature | What it does | Class | tdraw |
|---|---|---|---|
| Speed lock (`.syncon lo hi`, `.syncoff`, host; `SpeedHack`) | Clamps inside `SetGameSpeed 0x490DF0` (hook `0x490DF9`). The +/- keys (`0x4965B3`, `0x496559`) **and incoming speed packets** stay in range. The default range is 0..20, so **the floor drops from stock's 1 to 0** even with no command. The code swaps its two limits | sim | none |
| Autopause (`.autopause`, host) | Injects a pause (`0x19 00 01`) when the game's first `0x2C` or `0x09` passes. An unpause from anyone but the host is replaced by a pause | wire (controls the sim) | the battleroom button, `sharedialog.cpp` |
| Ready to unpause (`.ready`, `.voteready`) | When every live player is ready, every recorder injects an unpause, so the unpause goes out several times | wire | the button, `sharedialog.cpp` |
| `.votego`, `.forcego` (host) | Battleroom only: mark watchers as clicked in (byte 157 of their `0x20`) | wire (lobby) | none |
| Commander warp (`.cmdwarp`, host) | The recorder carries the state (Rec2Rec `0x04`, `0x01`) and unpauses when every player is done; tdraw moves the commanders | sim | `commanderwarp.cpp` |
| `.f1off`, `.ehaoff`/`.ehaon`, `.tahookoff` (host) | Tell tdraw to block the F1 page, or to turn off its interface upgrade or TA Hook's autoclick | local rule | `iddrawsurface.cpp` and others |
| `.take`, `.takecmd`, `.give`, `.stopgive` | The taker's recorder forges one `0x14` give-unit per call, **as if the dropped player had sent it**, then rejects that player (`0x1B`, reason 6). `.give` lets a non-ally take. The 2026 dist rewrites it, and its source is absent | sim | arbitration, `TakeClaim.cpp` |
| Version byte | The recorder's version goes in byte 182 of the outgoing `0x20` and decides which recorder messages a peer gets. There is no challenge and no enforcement | wire | `ChallengeResponse` |
| Trainer scan | Every 3 s it tests about 25 code sites known to trainers, plus `main+0x37F2F` bit 7 (`+doubleshot`). Every 14 s it scans window titles. It reports over Rec2Rec `0x02` and chat | wire | none |
| `.report`, `.reportmod`, `.players`, `.date`, `.time`, `.status` | Chat reports: version; mod and unit-type count (a manual same-data check); player list; lag | local | `.crcreport` and relatives |

The trainer scan has a coexistence cost. A peer running `eplayx.dll` would report our patches as
cheats wherever one of our sites is on its list. Nobody has compared the list with ours.

#### B candidates: stock defects the recorder works around

| Workaround | What it does | Class |
|---|---|---|
| Host migration and keepalive | Forces the `DPSESSION_KEEPALIVE` and `DPSESSION_MIGRATEHOST` flags when a session opens (`idplay.pas:3746`). The host destroys a rejected player | wire |
| Guaranteed pause, speed and DT packets | `0x19` always goes guaranteed. With `.protectdt`, so does a packet that holds a DT's build-finished `0x12` and its killed `0x0C` | wire |
| `.fixfacexps` (off by default) | Drops each incoming damage `0x0B` for a unit this peer reported killed within the last 3000 ms of wall clock | sim; superseded by B4's incarnation |
| Timer resolution | `timeBeginPeriod(wPeriodMin)` for the whole process, never undone (`InitCode.pas:117`) | local |

None of these exists on our stack today, which loads no `dplayx` proxy. What their absence costs, host
migration above all, is not measured. [INFERRED]

#### D. Data the recorder feeds tdraw's interface

The two DLLs share a named memory block, `TADemo-MKChat`; tdraw's `DataShare` is a superset of it.
Several tdraw features have their network half in the recorder:

- **Camera sharing** (`.sharemappos`, on by default). The recorder appends `0xFC`(x, y) to outgoing
  packets whenever the camera moves, and enemies receive it too. tdraw draws allies' views
  (`maprect.cpp`).
- **Whiteboard markers** travel as Rec2Rec `0x00`.
- **The income panel** (`cinomce.cpp`) shows numbers the recorder parses out of `0x28` and `0x16`.
- **Replay camera follow** (`.lockon`).

Porting one of these tdraw features means porting its recorder half as well.

#### Recording and replay: tooling, not a port group

A demo (`.tad`) holds a header, then every packet this peer sent or received. The header records the
players, each player's last `0x20`, the unit-sync `0x1A` records and a CRC. Each packet is stored as
TA handed it over, with a millisecond delta, and TA's LZ is applied on top. ESC's 416 names its
autorecorded demos `.ted`, but `.record <name>` still writes `.tad`.

`Server.exe` replays a demo. It hosts a DirectPlay game with one drone per recorded player and
re-sends the stream, and the watcher's TA joins that game. The mods engine writes `mods.ini` so that
the replayer finds the right install. Stats go to a `.csv`.

**Privacy.** Every demo holds each player's DirectPlay address, which is their IP, XOR-42 obfuscated.
It also holds every player's team chat, because the recorder copies each player's chat to every peer
(`0xF9`). The 2026 dist encrypts demos ("v1 has no key escrow"). A recorder for our stack would be a
project of its own.

#### Not to port

- **`.units`.** Every recorder answers a player whose recorder is version 2 or older, **including a
  player with no recorder at all**, by forging damage packets on a random half of that player's unit
  slots.
- **`.fakewatch`, `.forcecd`.** The first hides a player's units from the others; the second rewrites
  byte 159 of every player record it processes.
- **`.base`, `.baseoff`, `.dobase`.** `.dobase` never runs, because its side test is always true.
- **The unit-sync rewrite.** It randomises the checksum of one unit type (CRC `0x92549357`), which
  disables that type.
- **`.panic`.** Pure pass-through, which also turns off every workaround above.
- **Diagnostics with no player-visible feature.** The error-log module list (`0x4D989B`). The thread
  marshaller, which installs nothing. The input hook: its command table is empty in shipped code, but
  `0x417B9B` is the clean hook if we ever want console commands.

### F. The COB getters

tdraw has no COB getters. Its one COB feature is a dispatch-table speed-up (`CobDispatchTable`,
ESC-only), and its 57 addresses belong to ESC's modified exe, not to 3.1. [VERIFIED]

Stock `get` (`0x480770`) and `set` (`0x480B20`) answer ids 1..20; for any other id, `get` returns 0 and
`set` does nothing ([engine map](exe-reverse-engineering.md)). The shipped recorder takes over `get` at
`0x480770` and answers eight more ids. It never hooks `set`. Id 73 jumps back into the stock handler
with another unit. [BIN, disassembled in the 2013 build, 416 and the 2026 dist]

| id | Name | Argument | Returns | ESC scripts |
|---|---|---|---|---|
| 32 | `VETERAN_LEVEL` | — | own kills × 100 | 2 (dead code) |
| 69 | `MIN_ID` | — | 1 | 289 |
| 70 | `MAX_ID` | — | 10 × the unit limit | 289 |
| 71 | `MY_ID` | — | own unit id | 254 |
| 72 | `UNIT_TEAM` | unit id | that unit's owner index | 82 |
| 73 | `UNIT_BUILD_PERCENT_LEFT` | unit id | stock `BUILD_PERCENT_LEFT` (`0x480A44`) run on that unit | 260 |
| 74 | `UNIT_ALLIED` | unit id | 1 if its owner is allied to the caller's owner | 243 |
| 75 | `UNIT_IS_ON_THIS_COMP` | unit id | 1 if its owner is a human or AI **on the peer running the script** | 188 |

**The census.** ESC has 548 unique scripts, and 289 use at least one of these ids. No ESC script uses
any other id above 20, and none uses a `set` id above 20. Stock content uses ids 1..20 only. [a
bytecode scan of every script in ESC's eight archives and in the retail ones] The 2013 build answered
id 68 where 416 answers 75; ESC never uses 68.

**The other mods players run use them too, on fewer units.** The same scan over each mod's main
archive, as TA Forever installs it ([the suite's fixtures](compat/setups.md)):

| mod | scripts | use 69–75 | which units, which ids |
|---|---|---|---|
| Total Mayhem 11.3.0 (`mayhem.gp3`) | 495 | 8 | the moho mines and geothermals, `ARMHERC`, `CORMAT`: 69, 70, 71, 73, 75. Four mines also `get` and `set` id **111** |
| ProTA 4.8 (`ProTA.gp3`) | 311 | 0 | — |
| TA Zero Alpha 5 (`TAZ31.gp3`, build 120526) | 269 | 19 | the air constructors, the dropships and the `_ai` factories, all three sides: 70 and 74 only |
| TA Twilight v2.0 Beta 98 (`rev31.gp3`) | 518 | 10 | the galactic gates `ARMGATE`/`CORGATE`: all seven; the transports `ARMBVALK`, `CORBTRANS`, `ARMTHOVR`, `CORTHOVR` and `ARMMANT`, `ARMTSPD`, `CORSCORPI`, `BUNKER`: 70, 71, 72 |

Id 111 is `SET_CLOAKED` in the recorder's 4.0 `COB_extensions`, which no shipped build registers
(below); Mayhem ships the 3.9.2.416 recorder, which answers 32 and 69–75 and never hooks `set`
[INFERRED from its version string: the 416 getter is disassembled in ESC's build], so those four
mines read 0 and set nothing under TADR as well. None of the mods uses another id above 20.

**Without the getters these scripts run and do nothing.** Stock `get` answers 0 for an id above 20.
A scan then reads `MIN_ID` = `MAX_ID` = 0 and visits slot 0 alone, every unit reads as not allied
(74) and not on this machine (75), and 71 and 72 read 0. Escalation's and Twilight's gate
`Teleport` (one script, the same word offsets in both) skips every slot `74` does not call allied
(word 2656) before it looks at anything else, so **the gates never teleport**, and the indicator
pieces id 75 shows never appear. [the bytecode of `armgate.cob` in `TAESC.gp3` and in Twilight's
`rev31.gp3`; not run in a game]

**How ESC uses them.** Scripts loop `for id = MIN_ID .. MAX_ID` over every unit slot, every 0.5 to 3 s
per unit. They keep finished units (`73 == 0`), allied or enemy (`74`), other than themselves (`71`),
then read them with stock `UNIT_XZ` and `UNIT_HEIGHT`. Big Bertha, for example, shortens its reload
next to allied support buildings, and a factory turns its exit away from enemies. Id 75 confines work
to the owner's machine: indicator pieces, and actions such as `ATTACH_UNIT`/`DROP_UNIT` in `Upgrade`
and `Teleport`. Under TA's per-owner replication that is the right shape: only the owner acts, and the
result travels as ordinary unit state. [INFERRED]

**What a port has to fix, not copy.**

- 416 reads any slot with no alive test; stock's own handlers test unit `+0x110` bit 28.
- `UNIT_TEAM` and `UNIT_ALLIED` with an id above 10 × the limit read `nil+0xFF`, and 416's getter has
  no exception handler.
- Id 73 out of range returns the unit array's address, a non-zero value.
- `MAX_ID`-inclusive loops may touch one slot past the array.
- Every scan reads every unit on the map, regardless of fog.

The getter entry is one detour and eight handlers. Safety requires validating the original
full-width id against the unit array's count before narrowing, the alive bit and player index,
and the lifetime of every dereferenced object. Complete mod compatibility also needs safe
interpreter capacity, validation and saved state; it is not just this entry hook.

**The port is approved** (2026-09-28): [group F's decisions and gates](tadr-port/cob.md).
The eight getters, safe execution capacity and saved script state are in scope; id 111 is not.
Every mod players run with TADR except ProTA calls these ids somewhere, and every one of them
now runs on our stack with its own exe and archives, TADR's code kept out
([the takeover](compat/takeover.md)). F's core has landed, so those scripts now receive the
getters' values; feature parity across the mods is still being verified ([F's gates](tadr-port/cob.md#verification-gates)).

**A stock defect the census found.** `PUSH` (`0x4B13CF..0x4B13D9`) increments the stack index and
writes with no bound, and a thread record holds 32 stack words ([engine map](exe-reverse-engineering.md),
*The eight records*). ESC's CORDECI and ARMCRAWL `Detect` need 85 words, because their long `||` chains
of `UNIT_HEIGHT` tests keep every result on the stack; ARMVCAR's needs 54. The excess overwrites the
next thread records, or runs past the eighth into the COB object. Stock content peaks at 11 words. This
is a static finding, not reproduced in a game. It is a group-B candidate, fixed by a bound, and it does
not depend on the getters.

### The 4.0 prerelease — never shipped

HEAD registers about forty plugins, and **none of the ones below is in any shipped build**. A block of
them registers only when the mod's ini has `[MOD] ID` above 1; `TAESC.ini` has no `[MOD]` section.
Escalation's content uses none of their data keys (0 of 549 FBIs) or script callins (0 of 550
scripts). [VERIFIED, BIN]

| Units | Features | Gate at HEAD | Class | Note |
|---|---|---|---|---|
| `UnitInfoExpand`, `UnitActions`, `UnitSearchHandlers`, `SideDataExpand` | the `ShieldRange` engine shield, teleport order and button, action button, VTOL resurrect and capture, forced Y position, 25 new FBI keys, 4 new sidedata sections | keys always read; the consumers need `[MOD] ID` > 1 | sim + data | ESC's shields are pure COB |
| `COB_extensions` at HEAD, `ScriptCallsExtend`, `MaxScriptSlots` | about 129 getters and setters (create, kill and give units, orders on any unit, map scripting, local UI), new callins (`WeaponHit`, `TookDamage`, …), 64 script slots | always / `[MOD] ID` > 1 / ini | sim | the ids were renumbered up to seven times before 2015 |
| `OrdersOverride`, `Builders`, `Transporters`, `WeaponAimNTrajectory` | teleport rewrite, resurrect patrol, mobile builders placing anything, plants building on themselves, AI nukes, multi-unit and weight-capped air transport, high trajectory, `notairweapon`, `intercepts` | `[MOD] ID` > 1 | sim + data | Transporters' ground overload fix is already a B candidate ([sim-fixes.md](tadr-port/sim-fixes.md#open-questions)) |
| `MapExtensions`, `GAFSequences` | map mission scripts (a map `.cob` and `.tdf`), terrain swap, solar strength, a custom effects GAF | `[MOD] ID` > 1 | sim + data | ESC ships none |
| `WeaponsExpand` | 4096 weapon IDs, six weapon tags | tags always; IDs with ini `WeaponsIDPatch` | data; sim + wire | IDs: done by our A′3; the tags are unused |
| `NanoFrameUnits`, `BroadcastNanolathe` | build-cursor ghost and queued ghosts; broadcast nanolathe spray | always / ini | local / wire | our build ghost has landed |
| `GUIEnhancements`, `ClockPosition`, `BattleRoomEnhancements`, `SkirmishEnhancements`, `Colors` and `KeyboardHook` at HEAD | unit bars, true income, select boxes, range rings, scoreboard, clock position, battleroom buttons, idle-factory and share hotkeys | always / ini | local | D takes these from tdraw, which re-implements several at the same sites |
| `SaveGame`, `StatsLogging`, `RegPathFix`, `ExtensionsMem`, `Developers`, `SaveUnitsWeaponsList` | infrastructure for the above | always / ini | local | no |
| `KillDamage`, `TAExceptionsLog`, `MinimapExpand`, `PlayersSlotsExpand`, `StartBuilding`, `LoadWeapons` | not registered even at HEAD | — | — | the first two's claims are in [B's evidence §7](tadr-port/sim-fixes-evidence.md) |

**If ESC content is ever a target, look at ESC's exe, not at these plugins.** ESC's hex-edited
`TotalA.exe` carries behaviour of the same class as `Builders`, and its content depends on it:

- Mobile builders can place mobile units (`0x41AB62`, the same instructions as `Builders`, without its
  nuke exception); 180 build-menu entries rely on it.
- Mobile units get a yardmap (`0x42CF38`: `jne` becomes `jne +0`); 317 mobile FBIs carry one.
- Mobile nanoframes use nano colours (`0x45961A`).
- The AI builds nukes and anti-nukes, the Patch Loader's hack at `0x4FC980`.
- Heal-time units do not heal while being built (`0x48AF50`).
- The ballistic solver's branch at `0x49AA53` is flipped for every weapon. [INFERRED: a high-arc
  fallback for all ballistic weapons; not measured in a game]

[BIN, ESC's exe against the pristine one]

### Where the two DLLs overlap

In the shipped builds, one feature is implemented twice and two more patch the same site. [BIN]

| Feature | tdraw | recorder, shipped | recorder, 4.0 only |
|---|---|---|---|
| unit limit | `LimitCrack.cpp` (4-byte writes) | `UnitLimit` (2-byte writes; 1500 hard-coded in 416) | — |
| weapon IDs ≥ 256 | `WeaponIdOverflow`, `WeaponFiredExt` | none: it only parses the widened `0x0D`/`0x0E`/`0x0F` sizes, for recording, when `mods.ini` sets `UseWeaponIdPatch` | `WeaponsExpand` |
| per-weapon TDF tags | `WeaponTags`, `NotToAir` | — | `WeaponsExpand`, `WeaponAimNTrajectory`, at tdraw's sites `0x49AD07` and `0x49D702` |
| the pause key | `LagSwitchGuard` hooks `0x496099` | `PauseLock` hooks the same site, but never writes it | — |
| nanolathe colour | `TeamColorNanolathe` hooks `0x473F3B` | `Colors` (2026 dist only) writes the operand at `0x473F3D`, **inside tdraw's hook** | — |
| reload bars | `ReloadBars` at `0x469CB1` | — | `GUIEnhancements`, same site |

**Features split across the two.**

- The unit transfer is the recorder's; its arbitration is tdraw's `TakeClaim`.
- Autopause and ready-to-unpause are the recorder's logic behind tdraw's battleroom buttons.
- Commander warp: the recorder carries the state, tdraw moves the commanders.
- The recorder carries the whiteboard markers and the camera positions that tdraw draws, and the
  numbers tdraw's income panel shows.
- tdraw owns the CHAT_05 hijack channel (`PacketChatRouter`, `ChatHijackIds.h`), the vote-to-reject
  transport (msgId `0x2c`) and battleroom command dispatch.
- Voting is split by kind. Vote-to-reject is tdraw's (`VoteDialog`, `VoteReject`). The recorder's
  `.ready`/`.voteready` unpause the game, and its `.votego` marks watchers ready in the battleroom.
  `Voting.pas` is not compiled into any build.

[VERIFIED, BIN]

**Why it's in both:** `TotalA.exe` imports three Cavedog-era DLLs (`ddraw`, `dplayx`, `winmm`), and
each was hijacked as an independent entry point by a different community in a different language.
Each proxy had to be self-sufficient. The recorder shipped alone next to vanilla TA for a decade before
`tdraw` existed, so it bundled its own limit, line-of-sight and COB patches rather than depend on a
DLL that wasn't there. The two merged into one repo (~2019–23) but never into one program. See
[deep-tadr.md](deep-tadr.md).

## Merge & rewrite scope

Three ways to get TADR's feature set into the tagpu stack.

**Way 1 — use both at once (chain).** Point the exe's import at `tdraw.dll`; let it chain-load our
tagpu renderer as its `ddraw.dll` backend (the slot cnc-ddraw fills today). tdraw does game logic
plus its own UI through DirectDraw surfaces → our renderer; its `DllMain` and ours both patch, at disjoint
addresses (we own draw leaves; TADR owns sim tick / loaders / combat). One swap, no fork.

**Way 2 — merge into one DLL (vendor).** Compile TADR's self-contained sim/engine modules (all
`#if`-gated `Install()`/`Shutdown()`) into our tree under MIT, reusing `tamem.h` and `hook/`. One
proxy, one `DllMain`, one patch-address registry.

**Way 3 — rewrite it ourselves (review-and-improve each feature).** Tractably small, because the
expensive part is the render-side UI we already replace, and the sim/engine set is ~14 small +
~11 medium hook/patch modules. The infrastructure is already ours — see [gpu-status.md](gpu-status.md)
and [exe-reverse-engineering.md](exe-reverse-engineering.md). Exclude the megamap throughout.

**Conflict audit (before either way):** the dual-proxy roles; the `EngineLimits` projectile/explosion
pool relocation (our `tagpu_fx`/`tagpu_native` read those pools by pointer and must follow it);
UI ownership (our G15 vs TADR's megamap); Class-B patches that must ship identically to every player;
`tamem.h` struct pins vs our own offsets; config-key collisions; the MIT chain.

**Recommendation:** prove it with Way 1 (one swap, full fidelity), then graduate the sim/engine
features into Way 2 or Way 3 for long-term ownership. Rewriting (Way 3) is where the duplications
and the recorder's features collapse into one owner each — which means inventorying the Delphi side
too, not just `tdraw.dll`: [the recorder's shipped features](#the-shipped-features-by-port-group).

**Taken: Way 3.** [The TADR port](tadr-port/overview.md) rewrites the features group by group, under
standing rules the owner set on 2026-09-23 — TADR's multiplayer behaviour, fail-closed installs, no
runtime opt-out for anything that changes the simulation. Group A is done, in
[the section-A plan](tadr-port/raised-limits.md) and [A′. Content IDs](tadr-port/content-ids.md);
group B is planned in [B. Simulation bug fixes](tadr-port/sim-fixes.md) and group C in
[C. New data keys](tadr-port/data-keys.md); groups D and E are not planned yet. The state of
every group is in [the port's groups](tadr-port/overview.md#the-groups). The recorder's shipped
features are sorted into the same groups above, plus a group F for its COB getters.

## Sources

`github.com/tanvanman/TADR` — `src/DDraw/tdraw.txt`, `config.h` + `config_*.h`, the module set under
`src/DDraw/`, `src/Recorder/Plugins.pas`, `src/Recorder/plugins/*.pas`,
`src/Recorder/{Dplayx_exports,PluginEngine,InitCode_CoreExePatching}.pas`, `src/Server/`.
The recorder: `src/Recorder/{idplay,CommandHandlerU,IniOptions,ModsList,MemMappedDataStructure}.pas`,
`AddCommands.inc`, `Misc_Commands.inc`, `self_commands.inc`, `server_commands.inc`, every unit in
`plugins/`, and the history of each (`git log -- <file>`, back to `7886e2d`, 2013-04-18). The shipped
binaries: ESC GOLD 10.2.0's `eplayx.dll` (3.9.2.416), TADR's `src/Recorder/dist/escalation/eplayx.dll`
(2026.9.9-esc), and the 2013 build committed at `7886e2d` (`output/Dplayx.dll` with its `.map`); each
read through its `PACKAGEINFO` unit list, its string literals, its patch-site immediates and a
disassembly of its COB handler. Content: every `.cob` and FBI in ESC's eight archives, and the retail
scripts as the baseline. Every patch site named in the recorder section was disassembled in
`pristine/TotalA.exe.pristine`; ESC's builder patches were compared against it in ESC's `TotalA.exe`.
Cross-references: [deep-tadr.md](deep-tadr.md), [deep-ta-esc.md](deep-ta-esc.md),
[binary-patches.md](binary-patches.md), [runtime-injection.md](runtime-injection.md),
[project-map.md](project-map.md).
