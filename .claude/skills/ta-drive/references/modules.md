# Modules with their own workflow

Extra weapons, the COB trace, multiplayer, many unit types, weapons past 256, the new data keys,
the render-options screen with its GPU row, a remote Windows machine, a mod whose exe loads
Impure from its own ddraw proxy, and the demo recorder. Each is driven through `tacli`; what
differs is the setup around it.

1. [Extra weapons](#extra-weapons)
2. [The COB script trace](#the-cob-script-trace)
3. [Multiplayer: two to ten instances in one game](#multiplayer-two-to-ten-instances-in-one-game)
4. [Many unit types: the synthetic mod](#many-unit-types-the-synthetic-mod)
5. [Weapons past 256: the test weapons](#weapons-past-256-the-test-weapons)
6. [The new data keys: the key fixtures](#the-new-data-keys-the-key-fixtures)
7. [The render-options screen and the GPU row](#the-render-options-screen-and-the-gpu-row)
8. [A remote Windows machine](#a-remote-windows-machine)
9. [A mod that loads Impure from its own proxy](#a-mod-that-loads-impure-from-its-own-proxy)
10. [The demo recorder: recordings and dumps](#the-demo-recorder-recordings-and-dumps)
11. [Resource snapping and Quick Build](#resource-snapping-and-quick-build)

## Extra weapons

`tagpu_weapons.c` lifts "three weapons per unit" to `Weapon4..N` (capacity 16). It is **always
on**: its sites are in the fail-closed table, so every launch has it, `--defaults` or not, and
stock units run the untouched engine code (`research/notes/extra-weapons.md`). The launch log
says `weapons: 44 sites to the fail-closed table`, and the table's `limits: installed N sites`
line (`the simulation fixes' N sites installed` in the stock-limits build) says they went in; an
exe that differs at one of them is refused at start, the site named.

```bash
tools/tacli launch w1; tools/tacli stop w1          # create the instance dir
tools/extra_weapons_fixture.py                       # builds scenarios/content/wpn-test.ufo (gitignored)
ln -s $PWD/scenarios/content/wpn-test.ufo tagpu/instances/w1/gamedir/   # the test units
rm -f tagpu/instances/w1/catalogue.json              # cached type list is now stale
tools/tacli scenario load w1 wpn-llt10 --restart     # two ten-laser towers vs solars
tools/tacli weapons w1                               # every slot of every unit + counters
tools/tacli log w1 -g "weapons: (unit-data|loader|reload|VIOL|MISM)"   # each load empties, then fills
```

- **`tacli weapons <inst> [idx…]` is the oracle**: per slot the state byte, weapon, target,
  reload, heading, pitch, stock, aim result and COB thread, plus the C-path hit counters,
  projectile launches per slot, and each extended type's cached `AimFromWeaponN,QueryWeaponN`
  pieces from slot 3 on (`-1` = not asked yet). With stock content every counter but
  `loader`/`stock_splice` must read 0 and `mismatch`/`violation` must be 0; that is the
  regression check.
- It also prints each unit type's `CRC_weapons` and `CRC_all`, the unit-sync values. A build
  without the module (the retail game) differs for exactly the types carrying `Weapon4+`. In a
  multiplayer game TA **disables**
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

**Extended-getter probe:** `python3 tools/cob_getter_probe.py --out /tmp/cob-getters --expect implemented --extended --roundtrip --quarantine`
creates its own isolated Escalation setup, adds a generated probe type and calls all eight
required getters on human-owned and local-AI-owned units. It asserts values from the return
trace, inspects the live exe for TADR hooks, saves evidence outside the repo and cleans up its
instance/display. `--extended` checks full-width invalid IDs, nonzero kills/construction,
local 100 and every argument of a 100-argument CALL. `--roundtrip` saves and restores sleeping
expanded records; `--quarantine` checks type exclusion, native chat and a presented-window
capture; `--reject-required` expects a required-unit refusal. Separate
`--thread-exhaustion start|call` fixtures fill all ten guarded records and exercise refused calls
with a full 128-word argument stack. START consumes refused arguments and resumes
after SIGNAL frees slots; CALL retains the arguments, leaves its parent dormant
on child `-1`, and checks that the match keeps advancing.
`--expect stock-zero` is the negative API control.
`--skirmish-ceiling` fills player three's native block and asserts live ID 6000 in a
15001-slot pool. It configures ESC's own `TAESC.ini` and `Software\TA Esc` registry root,
not retail's settings. Skirmish activates only four players; this is not evidence for
global live ID 15000, which requires a network fixture.
`--unused-model-pieces --extended --roundtrip` keeps 4096 declarations over the solar model
without accessing the excess and checks that saving/restoring still runs the getter suite.
Adding `--reload` loads a marked replacement through native chat before creating the probes;
the driver observes its unique code tail, then applies the same getter/save assertions.

`tools/cob_corpus_probe.py SETUP... --out DIR` audits effective native-loader choices in
paused isolated games and preserves metadata/hashes, not game scripts. A nonzero exit reports
quarantined types, including malformed content bundled with a setup; it is not silently
waived. `tools/cob_safety_probe.py divide piece sweetspot native-ops --out DIR` drives
diagnostic refusal, model-bound type exclusion and native instruction continuation.

`tools/cob_peer_probe.py SETUP --port N --out DIR` checks self and remote ownership on two
peers; `--quarantine both|host|join` checks matching malformed files or native mismatch
exclusion. `tools/cob_feature_probe.py --out DIR` runs a real Escalation fusion upgrade,
including an in-progress save/load and real high-stack scripts. `tools/cob_legacy_save_probe.py`
creates a native-32-word save and imports it with the current DLL. See each tool's `--help`
and `research/notes/tadr-port/cob.md` for measured coverage. Output directories must be new
and outside the repository; all drivers clean up their owned games/displays.
`tools/cob_mission_probe.py --out DIR` checks refusal for a malformed placed ARMFAV in the
first Arm campaign; `--control` leaves scripts intact and requires all three placed Jeffys.

The trace's `H` event records new per-record stack peaks above 32. Argument lines retain all
128 possible words. A trace containing `INCOMPLETE` cannot establish behavior: its bounded
per-thread tracing capacity or thread-local storage was unavailable. Starts and argument
reads are ordered against the COB destructor; no freed record is read for a delayed trace.

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
tools/mp_leave.sh h1                                 # surrender -> main menu
```

Use instance names of at most ten characters with `mp_lobby.sh`: it uses the
uppercased instance name for TA's ten-character nickname field.

The recorder's isolated native backend has a standalone transport fixture:
`python3 tools/replay/recmp_net_test.py --port 47733 --out /tmp/recmp-net-run`.
Add `--channel` to exercise the production channel, initialized after both primary
interfaces close, including roster/configuration copying, signed human/AI bindings,
source sequences, source-prefix/final seals and packet delivery. The default exercises the backend directly,
including its sixteen completion-context bound and refusal of an early Close.
With `--channel`, `--body-bytes 1..4032` varies the copied DATA body (default 16);
4032 exercises the maximum 4096-byte application packet with its 64-byte header.
`--trace-network` requires strace and retains `host.trace.*` and `join.trace.*`
from only the owned native peers and their descendants. Successful kernel network
calls show datagram framing and fragmentation below the SDK. They include Wine
IPC and discovery traffic: do not label all traced bytes as private contributions.
Tracing perturbs timing; the logs establish neither physical wire bandwidth nor
undisturbed game latency. Use the maximum-payload probe as follows:

```bash
python3 tools/replay/recmp_net_test.py --port 47733 --channel \
    --body-bytes 4032 --trace-network --out /tmp/recmp-wire-run
```

Both modes run authentication, stream-ledger bounds/corruption/missing-tail tests,
and controlled channel bounds/gap/wrong-source/missing-tail/abandoned-drain tests.
The runner also checks frozen consumption cuts, final receipts, retained tail
draining and source/application journal wrap/overflow. The production-channel
fixture keeps separate `release.host.journal` and `release.join.journal` packet
histories; these are fixture journals, not playable `.imprec` recordings. Missing-receipt and missing-cut controls each take the production
deadline and must fail incomplete, as does the abandoned-drain control; elapsed
time cannot prove successful receipt.
Run this standalone fixture separately from menu-driving tests: its instance removal can race the global `tacli ls` inventory used by `mp_lobby.sh`. Choose a free discovery port and a new output directory. The runner creates and
removes two private-registry prefixes and an X display, retaining logs and test
executables. It uses primary-session stand-ins; it does not launch TA or verify
terminal recording completeness, bandwidth, recording or playback.

For a single coverage completion, create `talog.Cursor(gamedir)` before writing
`cover <name>` to `tagpu_replay.trigger`, then observe its `text()` and copy the
completed file. `tacli wait` observes only lines written after it starts; starting
it after the trigger can miss the one completion line. Retain the tested COVER
DLL and its matching linker map beside the collected bitmaps.

The contribution file container has a separate controlled fixture:
`python3 tools/replay/recmp_file_test.py --out /tmp/recmp-file-run`.
It uses the actual world-file writer, compressor, lock and C/Python readers in one
owned prefix/private display, without launching TA. It writes format-6 synthetic
`.imprec` files, validates source/application/proof records and the frozen-cut
footer, seeks the synthetic world, and refuses missing proofs, local-save failure
and logically corrupted packets/footers. `--native-file <recording>` checks an
actual complete native file with C/Python application-byte/order/provenance
agreement, all owner players/unit-block bounds and world seeking; this reads
copied state without engine playback. `--read-file <fixture.imprec>` runs the
reader controls against an existing fixture file; `--skirmish-file <recording>` also checks an actual skirmish header/summary/info. The mode-3 header carries copied origin/seat/unit-block mapping, with world allocation and seat ownership validation; malformed mapping controls rebuild outer CRCs before expecting refusal. The typed fixture verifies source-roster carriage, complete eleven-row remapping,
proof-only roster isolation, and reader player/unit/order authority queries through playback/seek, mapped owner/attacker rows, known absence versus unknown slots, exact nonempty list bytes and provenance, partial-list retention and proof-tail isolation. Body controls rebuild source seals as well as outer checksums before semantic refusal. These files do not prove game
capture, full owner authority, engine authority application or multiplayer playback.

The complete normalized player-image contribution has a bounded-cache fixture:
`python3 tools/replay/recmp_players_test.py --out /tmp/recmp-players-run`.
It uses one owned prefix/private display without launching TA. C exercises frozen
origin/seat/block bounds, complete human/AI images, deltas, source/application
provenance and guard-page truncations. C also checks copied-image player-row mapping:
640 roster permutations, 3,520 exact inverse round trips, identity and fixed-block
agreement, retained vacant/dummy entries, in-place mapping and unchanged-output
refusals. The immutable source-map cache checks first-sequence/version/stream prerequisites;
wire version 2 contributes a copied roster before player/order state. File authority
queries map player vectors/self rows to recorder coordinates. C and Python independently reconstruct
the four exact images from a raw contribution journal and refuse corruption or
a missing tail. It does not test game `.imprec` playback, seek or AI takeover.

Copied unit/mover images have a separate bounded-cache fixture:
`python3 tools/replay/recmp_units_test.py --out /tmp/recmp-units-run`.
One owned prefix/private display runs C controls without launching TA: 640 roster
permutations and 464,640 exact inverse mappings across every bounded attacker row
and nullable player ID; sixteen unchanged-output mapping refusals; full/delta,
type/birth/death, provenance and unknown-versus-absent checks; twenty-seven atomic
batch refusals. Guard controls cover all 372 truncations of a mixed full-live and
absent packet, exact spans, a maximum 201-entry batch, source-map/cache allocation
disagreement and the maximum 65,531-slot allocation's final-slot bounds. The file
fixture independently checks mapped unit authority and proof-tail isolation in
C/Python through synthetic seek. These fixtures do not test engine application,
owned COB/model/effect objects or native multiplayer recording/playback.

Owner begin/create/death/return/script/reply/spray contribution bounds use:
`python3 tools/replay/recmp_roots_test.py --out /tmp/recmp-roots-run`.
One owned prefix/private display runs copied-cache and 4,096-call queue controls:
83 atomic refusals, all 1,300 guarded batch truncations, 485 per-kind short spans,
maximum 167 replies/251 death returns and 8,392 mixed
calls across packet splits/actor switches/queue wrap. Overflow, backwards source
time or failed emission permanently refuse. Payloads preserve exact argument/text/
spray/constructor bytes, source clock, incarnation token and pre/post-capture phase.
Constructor payloads retain the source owner row, validated through its frozen
source roster, and all finished/position bits. Contribution-only constructor
begin `0x100` precedes initialization/child roots; ordinary CREATE is the success.
Death payloads keep the complete normalized victim/mover image and source owner
indices; four-byte returns retain the saved exact incarnation. These brackets
run with recording off and return follows no freed engine object. The file fixture checks ordered
applied mixed batches through synthetic seek and proof
isolation. Native constructor ordering uses the coverage DLL:
`python3 tools/replay/recmp_create_begin_test.py --port 47818 --out /tmp/recmp-birth-run`.
`--reproduce` installs `tools/reproduce_fixture.py`'s archive (the stock trees and shrubs with
`reproduce=100`, generated from the install, written only into the instances and `--out`) on
both peers, and `--until T` keeps the game running to GameTime T before the host leaves: the
feature cursor visits one cell per tick from the map's last cell, and on Two Continents it meets
a tree at rest at tick 387, then none until 1577, and 18 by 2000 (`--until 2000`).
`--chat` has the two humans talk once the roots are in, through each one's `TALK` box: a public
line from each and a private line each way (`1:` picks the other human), so the file carries
originals and receipts for every audience a two-human game has.
For copied owner lifetime dispatch from a saved file, use
`python3 tools/replay/recmp_lifecycle_test.py --native-file <recording> --out <directory>`.
It owns one isolated Wine prefix/private display and launches no game. C/Python
check exact action order/provenance, incarnation dependencies, normalized owner
fields, proof-tail isolation and fresh reconstruction after reader seek. This
checks the dispatch layer; it does not establish native engine playback or Seek.
For fixed global owner blocks, use
`python3 tools/replay/recmp_arena_test.py --out <directory>`.
It owns a private Wine prefix/display and launches no game. It checks copied
permutations and executes a relocated retail unit allocator, replacing only its
heap calls and retargeting verified absolute operands. All ten paired ownership
blocks are checked; multiplayer loader admission and saved playback are not.
It creates two fresh instances on a private display, adds one hosted AI per
human, records on the host while the peer's saving is off, compares source and
recipient first-root bytes, and checks exact begin/child/success order in the
completed file. It retains that file, logs, DLL/map and coverage, then stops and
removes only its own instances. Run the saved file through
`python3 tools/replay/recmp_file_test.py --native-file FILE --out /tmp/recmp-birth-read`
to compare complete C/Python application delivery through reader world seek.
These are native capture/reader tests. Saved multiplayer playback is measured with
`python3 tools/replay/recmp_play_test.py FILE --out <directory> [--at T ...] [--to-end]
[--perspective N] [--content ARCHIVE ...]`, which plays FILE on one tareplay worker at speed 20,
dumps at each T and exits through the game so the playing's counts are logged (`--content`
copies an archive the recording was made with, such as the recording run's `zzrepro.ufo`, into
the worker for the playing and removes it after). The same script seeks:
`--roundtrip T` (dump A, poison, seek, dump B; compare the pair with `tarec.py
compare-dumps`), `--seek A:T` (at GameTime A seek to T in the same process,
dumps s<T> and s<T+120>), `--fresh T` alone (a fresh process at T, dump
q<T>), or `--takeover T:P` alone (a fresh process takes over at T as P, dump
k<T+120>, then tareplay's AI-activity check; the file with its timeline is
kept as `fork-<name>`; `recmp_compare.py --fork 1 FILE` compares the fork's
first iteration with the composed parent). `--timeline N` plays or seeks
timeline N's chain; `--hide-chat` writes `chat hide` once the playing starts
(`recmp_compare.py --chat VIEW[:SINCE] FILE DUMP...` checks a dump's chat lines
for the viewed record, `full` for the full map); then
`python3 tools/replay/recmp_compare.py FILE <directory>/dump-p<T>.bin ...` compares each dump
with the owner-authority world the recording implies (the multiplayer slice note's
*Owner-authority materializer*). The fixture requests speed 20; count it as 2x evidence only if live
heartbeats confirm 20. The retained legacy named-only codec is exercised by
`python3 tools/replay/recmp_named_test.py --out /tmp/recmp-named-run` (28 atomic
refusals and 94 guarded truncations). Codec/file controls do not launch TA;
neither they nor the native constructor fixture establish engine root application.
Run owned-prefix codec/file fixtures
separately from multiplayer menu drivers, as for the channel fixture above.

Native multiplayer can start at speed 10 despite a stopped-instance setting of
20. Check the in-game `packet:` heartbeat's `speed` and `tps` before labelling a
network run accelerated. Tick-based skirmish replay fixtures use 20; multiplayer
timing and bandwidth tests retain their own speed policy.

Per-origin explored-map contributions use:
`python3 tools/replay/recmp_explored_test.py --out /tmp/recmp-explored-run`.
The owned-prefix codec fixture checks ten permuted origins, copied encoder input,
multipart full/delta atomicity, mode-only updates, malformed/guarded spans,
unused tail bits and failed emission. The file fixture also checks bitmap/mode/
provenance queries through seek and proof-only isolation. With `--native-file`,
it compares C/Python exploration queries and local-origin snapshots against the
recording's independent raw primary map at the same B2 tick. These tests do not
enable native engine playback or complete POV capture. In a live game,
`recmp: local explored map` / `remote explored map` logs the first complete
bitmap's origin, source sequence/tick, application tick, geometry, LosType and
CRC; match source/recipient by origin, with each side's application clock
retained separately. Run the standalone prefix-removing fixture separately from
multiplayer menu drivers.

`python3 tools/replay/recmp_chat_wire_test.py --out /tmp/recmp-chat-wire-run`
checks copied identified text packets, repeated full-length text with distinct
serials, alias-safe copies, guarded truncated spans and malformed packet
refusal. It cleans up its owned prefix/display and runs separately from menu
drivers. The native `recmp: text source` / `text received` logs carry sender
identity, event serial, text-packet CRC and separate source/recipient ticks.
These logs do not establish persisted communication or replay playback.

`python3 tools/replay/recmp_text_test.py --out /tmp/recmp-text-run` checks
copied kind-12 text batches, all ten origins, receipt-before-original joins,
repeated/reordered receipts, full-length bytes, malformed/guarded spans,
identity/queue capacity and failed publication. C/Python copied file-history
controls check missing-original refusal, proof-only isolation and application
prefix reconstruction. This fixture removes its owned prefix/display and runs
separately from menu drivers. Native `local text batch` / `remote text batch`
logs identify contribution publication/consumption, not replay display. Source
entries include local echo and intended target calls; received entries belong
to the actual recipient's stream and clock. `recr_mp_text` inspects the latest
applied batch; every observation remains in the ordered application ledger.

Saved native chat evidence uses:
`python3 tools/replay/recmp_file_test.py --out /tmp/recmp-chat-read --native-text-file FILE --chat-evidence DIR`.
`DIR/roster.json` maps each instance name to its ten native player rows;
`DIR/NAME-tagpu.log` retains each human endpoint's source/received wire logs.
The fixture requires text observations, checks the complete footer and every
C/Python text entry, application bytes and latest-batch query through reader
seek, then compares author/serial/CRC and source/recipient clocks with those
logs. Authors and actual recipients must all have evidence, including a saving
peer that receives no private messages. Run this owned-prefix fixture after
the native menu driver has stopped. `--native-file` retains the separate
combat effects and independent primary exploration oracle gates. Neither
reader mode tests engine multiplayer playback or loading-phase delivery.

`python3 tools/replay/recmp_receive_test.py --out /tmp/recmp-receive-run` checks
the exact production primary receive core, receive-span API and movement copy
with controlled SDK/statistics boundaries: maximum u16 message, raw buffer
growth, independent native output,
nested storage/span/source-destination restoration, aliases retained after
return and depth-slot reuse, error/size refusal and two-thread receives. A busy
foreign caller must finish without consuming an SDK message while the owner
holds dispatch. The fixture also checks movement header clamping/padding and
retail fallback. It
cleans up its own Wine prefix/display. Use the native menu driver separately
to exercise the installed entry/call wrapper; these
controls do not prove loading-chat admission or native raw transport. When the
primary installer arms, `recmp: ARMED owned primary receive frames and serialized
dispatch` identifies the path. Identified chat accepts loading/play states 5/6
through a copied current-match inbox, including receipt before the B2 producer.

`python3 tools/replay/recmp_chat_inbox_test.py --out /tmp/recmp-chat-inbox-run`
checks the production copied inbox/text queue and exact receive adapter:
pending reset survival, once-only bootstrap, original zero/source clocks,
publication ownership, final transfer/admission closure before queue free,
concurrent receipts, overflow with stock restoration,
frozen-roster/match refusal and state-5 calls on game/foreign threads without a
producer. Main/mode/span/log boundaries are controlled; this does not prove a
native loading receipt, its display or persisted application clocks. Its own
prefix/display are removed.

`python3 tools/replay/recmp_primary_test.py --port PORT --out /tmp/recmp-primary-run`
checks native live movement through the installed primary wrapper in two owned
human instances, with saving off and a 20-unit limit. Both commanders must move
on both observers; native movement counts must be positive and stale refusals
zero. Add `--raw` for the unqueued primary transport. The fixture retains the
tested DLL/map/hash, native ticks, rosters and logs, then removes its own
instances/display. This does not test saved-file completeness, engine replay,
loading chat, bandwidth or continued migration.

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
- **Leaving a peer returns that peer to its main menu.** Inspect the other peer's screen: a four-seat game can remain in play after the host leaves. For a peer on `ENDMSN.GUI`, click `MainMenu`; for one still in play, run its own `tools/mp_leave.sh`.
- **`START` ungreys only when every player is ready, the host included.** Each client lists
  *itself* as row 0, so the host's own toggle is `READY0` on its screen and the joiner's is `READY0`
  on theirs. `PLAYER0`/`READY0`/`PLAYER1`… are created at runtime and sit past the end of the
  default `ui` snapshot; reach them with `ui <inst> show READY1`.
- **Lobby state syncs**: set the map on the host and read it back on the joiner
  (`ui <join> show MAPNAME`) as a cheap proof the link is live.
- **Hosted AIs occupy their own rows and native DirectPlay IDs.** Choose a map with enough
  seats (*Town & Country* takes ten), stop the lobby helper with `MP_NO_START=1`, and read an
  unused row with `ui <inst> show PLAYER2` before clicking it. On the host the row cycles
  `UNUSED -> [BLOCKED] -> AI:<host>`; on a joiner `UNUSED -> AI:<joiner>`. For two humans and
  one AI hosted by each, use host `PLAYER2` and joiner `PLAYER3`. Ready each human's `READY0`
  and start on the host. A hosted AI is controller 2 on its hosting peer and controller 3 on
  the other peer; match it across peers by `player+4`, independently of its host's human ID.
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
- **A unit's colour is its owner's logo colour on every peer**, not its slot: each peer lists itself
  as player 0, so a slot-indexed colour would draw everyone's own units alike. Check a colour by
  reading `PlayerInfo+0x96` of both players on each peer (the lobby's `LOGO0` gadget cycles the
  host's colour) and comparing the window with `tacli shot`; the two peers' windows overlap on one
  display, so raise the one to capture (`xdotool windowraise` on a private Xvfb only).
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
- **Wait for `TALK` after opening chat, then use `ui fill TALK <text> --json` and compare
  `text` with `wanted` before submitting.** The fill posts at most four characters per batch
  and waits for exact prefix acknowledgement; one snapshot can precede the edit. A differing
  field is a failed input attempt. Long direct `keys char:…` runs can fill the engine's
  character ring; token-file consumption does not prove a field edit.

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
  `fill GAMENAME <name>`, then `click LOAD` (the save screen's button is named LOAD). The field
  keeps only the first letter a `fill` types, so name the save with one letter. Loading is
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

## A mod that loads Impure from its own proxy

Escalation, TA Zero and TA Twilight ship an exe that imports the mod's own proxy, which loads Impure
from inside its start-up. **`tacli launch` cannot start them**: it adds `-xtacli-test` and creates
`tacli-state/`, and the DLL then refuses the launch because `win32.dll`, a static import of the
exe, is not loaded at attach. Drive them the way the compat suite does:

1. Build the instance from the suite's folder: `tacli create <n> --private-registry --display <d>`,
   then `rsync -a` the suite's `compat-<setup>/gamedir/` over it, leaving out `tacli-state`, `log`
   and `tagpu_*`.
2. Move the `gamedir/tacli-state` folder aside (the suite gives it an `.off` suffix; a launch
   recreates it, so do it after the last `tacli launch`), and copy the tree's `ddraw.dll` over the gamedir's.
3. Start `wine TotalA.exe` in the gamedir with `WINEPREFIX` set to the instance's canonical
   `prefix` path — `tacli` finds the game by that exact string, so a path containing `..`
   reads as "not running" — `WINEDLLOVERRIDES=ddraw=n,b` and `DISPLAY`.
4. `tacli ui`, `click`, `roster`, `shot` and `stop` then answer as for any instance; the play
   defaults are on because no `tagpu_defaults.off` exists. The window is the display's size.

## The demo recorder: recordings and dumps

**A skirmish or native multiplayer game is recorded while `tagpu_recorder.on` is in the game folder** — `tacli arm <i>
recorder.on` before the level starts, since the recorder looks for it at each level's first
iteration — from that iteration to the level's end, to `gamedir/replays/<date> <time> <map>.imprec`.
Without it the log says `recorder: not recording: tagpu_recorder.on is absent`. The log says
`recorder: recording …` at the start and `recorder: writer: … closed: …` when the file is done, with
the corrections, which must be 0; `recorder: N iterations captured: …` gives the capture's cost.
A native file contains this peer's observed world plus the separate exact-owner
contribution ledger. `tacli replay play` plays it with every actor a puppet beside the viewer,
`replay seek` and the `seek` trigger seek it, and `replay takeover` takes it over into a skirmish
timeline of the same file; it has no ten-seat hosting yet (the multiplayer slice note's
*Owner-authority materializer* and the sections after it).
Mandatory contributions run with the recording switch off and continue after a
local save failure. Live rewind is available only for a recorded skirmish.
Quitting waits for the file of a game that has ended — the in-game menu's `EXIT` then `EXITGAME`,
or closing the window after the level's end (`recorder: the exit waits for …`) — so that file is
complete. For exit coverage, the replay harness uses the game's menus: `ENDMSN.GUI`'s active
`MainMenu`, then the main menu's `EXIT`, or in play `EXIT` → `EXITGAME` → Yes. It waits for the
instance's process to end and collects the final bitmap. `tacli stop` sends SIGTERM and bypasses
that exit hook. A game killed mid-way, or whose window is closed mid-game, leaves a file without an end
block that reads to its last complete block.

- **Read one** with `tools/replay/tarec.py`: `info` (the header, the blocks, how it ended),
  `check` (the stream rebuilt and compared with every snapshot in the file) and
  `compare FILE DUMP…` (`--puppet` for a playing's dumps). Exit 0 when every check holds, 1 when
  one fails, 2 when a file cannot be read. `compare --images DUMP OTHERDUMP` is a report, exit 0:
  the bytes of the main block and `.data` that differ and that no row of
  `tagpu/ddraw/replay-globals.tsv` covers. `compare-dumps FILE DUMP OTHERDUMP` compares two
  playings' dumps at one GameTime, the sight counters included. Every dump also carries the
  counts of the engine's roots and sinks: `counts FILE DUMP` lists them, and `compare-counts FILE
  L_A L_B P_A P_B` compares a live game's (two of its dumps) with a playing's over the same
  window — roots exactly, sinks within `tagpu/ddraw/replay-absent.tsv`'s band less its rows, and no
  COB engine write or native owner-only script start on the puppet. `images-gate FILE L_T1 L_TLAST TWIN1 TWIN2 P_T1 P_TLAST R_T1 Q_T1`
  classifies every uncovered byte where the live game and a playing or a fresh seek differ, and
  `scan FILE DUMP` fails a plain value that is an address. A file holds one or more timelines
  (a takeover appends one): `segments FILE` lists them, `check` and `compare` take `--timeline N`
  to read timeline N's chain, and `compare-fork FILE N` compares timeline N's first iteration with
  its parent at the fork point, less the takeover's expected differences.
  `tarec.py` and the replay tools need numpy and zstandard and re-execute themselves under the main
  checkout's `.venv-undither/` when they are missing (a bare `python3` is fine). Every command that
  reads the stream decodes the whole recording in Python — about 30 s for a large battle — so set
  `TAREC_CACHE=DIR` when running several on one file: the decoded blocks are kept in `DIR`, keyed by
  the file's bytes and the decoder's sources (`tareplay.py` does this for its own runs).
- **Check playback end to end** with `tools/replay/tareplay.py run FIXTURE… | all [-j N] [--keep]
  [--game-speed 10|20] [--out DIR]` (`list` names the fixtures of `tools/replay/fixtures.json`):
  tick-driven runs default to internal speed 20 (2x normal); `--game-speed 10` selects normal.
  Each stopped owned instance's private registry store is configured before launch.
  Cost fixtures and W's speed/settings UI test use normal speed. Tick endpoints,
  round-trip windows and comparison rules keep their existing units; wall-clock
  deadlines are not shortened. Armed round trips bound the tick batch at T−1,
  so their frame checks cannot be skipped by faster simulation progression.
  Use normal speed for timing, network latency and bandwidth measurements.
  The same option applies to `measure`, `landing`,
  `broad`, `takeover`, `forks` and `rewind`. Each fixture is
  recorded live, played on puppets, played again with three round trips and sought from a fresh
  process, each on its own Xvfb; the live game also takes the null twin (paused, the camera, the
  wheel, the cursor and the in-game menu moved, two endpoints and intermediate paused
  views). Each view requires unchanged GameTime and exact carried-simulation equality.
  Mouse samples follow a GUI-consumed click with an explicit move and require its native
  x/y/message echo. Camera samples span the map's native scroll extents, including native
  clamping at the far edge; resource-refusals samples ARMACK and then RSNRACK selection,
  each with its native displayed-type acknowledgement and exact paused comparison.
  Every owned worker removes its cached `catalogue.json` when it
  replaces fixture content, so scenario validation never uses a previous fixture's units.
  `scenario load` with explicit players replaces the saved seat extent as well as
  controller keys; a prior playback's smaller roster cannot truncate the next fixture.
  A fixture with `"watch": true`
  runs a fifth process, W: the playing started from REPLAYS and Watch as a player starts it, two
  edited copies refused, then the dock's speed, the Options' OK, pause, the dock's seek, the
  player cards' Sight and Full map (clock and camera unchanged), its dump
  (`w<T1>` compared like the others), resume and Leave. It runs every check above and
  reports each by name: exit 0 all passed, 1 a named failure, 2 something could not run. It
  refuses a `ddraw.dll` (or `--stock-dll`) older than its sources — rebuild with `make -C
  tagpu/ddraw` (`LIMITS=stock` for the stock one) first. The run's folder is outside the
  repository (`--out`, `$TAREPLAY_OUT`); passing dumps are deleted unless `--keep`, and a dump
  any check failed on stays. `run --cover` runs the coverage builds instead. A fixture with a
  `takeover` entry also runs K (its recording taken over at T2, ordered, dumped and ended) and F
  (K's file played from T−300 across the fork); `tareplay.py takeover FIXTURE` runs the live game,
  K and F alone, with the checks `judge`, `line`, `orders`, `ai`, `fork` and `victory` its entry
  names. The `ai` check observes 1800 future ticks, armed under an acknowledged engine
  pause after takeover. Every AI seat must issue fresh planner orders that reach normal
  dispatch, and at least 25% of AI mobile units must move more than 8 wu. Construction
  is reported only; K waits for the observation to complete before ending the game.
- **Native fixture input** uses a `keys` step with a list of `tacli keys` tokens.
  `secondary-orders` and `stock-secondary-orders` select the isolated solar with
  `pclick:512,384`, then send `ctrl+d` in a later GameTime step to exercise the
  secondary order dispatcher. The camera is pinned by the scenario; the shield
  stays on so the engine sees the polled Ctrl modifier.
- **The replay gate of a landing** is `tools/replay/tareplay.py landing` (`--base main`, `-j N`):
  it runs the fixtures that the changed `SIM_FEATURE` regions name, on the coverage builds, fails
  `UNFEATURED` for a changed line outside every region and `UNCOVERED <file>:<function>` for a
  changed function no fixture reached (in each build that compiles the changed line: the
  stock-limits build's lines need `stock-limits`), and on a pass writes a `replay-roundtrip:` git
  note on the commit it graded. Commit first: it refuses uncommitted changes under
  `tagpu/ddraw`, `tools` or `scenarios`. `landing --check` says whether a passing note already
  covers HEAD.
  Changes to `tools/tacli` select every fixture, as changes to the replay harness do.
  `tareplay.py broad` runs every fixture and `fixtures.json`'s `broad` entries (`limits-tier1`
  with 20 round trips; `cost-four-ai`, ten minutes of four AIs reported for the recorder's cost
  and the file's size) and names what none exercised (a feature, a root or sink site, a recorded
  table); `broad --release DLL` runs a release DLL without reach, before a `v*`
  tag. `measure SCENARIO… [--end T]` records and plays any scenario on the coverage builds and
  lists the functions it reached, for choosing a feature's fixtures; `features` lists the regions
  and `features --check tagpu/ddraw` is the link's check.
- **The coverage builds** are `make -C tagpu/ddraw COVER=1` (`ddraw-cover.dll` and its map
  `ddraw-cover.map`) and `make -C tagpu/ddraw LIMITS=stock COVER=1`; `landing`, `broad`, `measure`
  and `run --cover` need both built and newer than the sources. In such a build, `cover <name>` in
  `gamedir/tagpu_replay.trigger` writes `replays/cover-<name>.bin`, the functions of the
  instrumented files that have run (`cover: … <n> functions that ran`), and writes it again at the
  level's end and after the exit has waited for file workers; any other build answers `cover: <name> refused: not a coverage build`.
- **A dump**: write `dump <name>` (letters, digits, `-`, `_`) to `gamedir/tagpu_replay.trigger`.
  The game reads the file at most every 100 ms on a level in play, recording or playing, deletes
  it, and takes the dump at the next tick: `replays/dump-<name>.bin`, the live world at that
  GameTime, logged as `recorder: dump <name> at GameTime <t> -> <path>`; `tarec compare` rebuilds
  the recording to the same GameTime and requires it byte for byte. A paused game takes it at the
  first tick after the pause. **`dump <name> at <T>`** takes it at GameTime T exactly, whatever the
  speed or pauses — the way to dump a playing at a given tick, since a puppet host's speed keys
  change nothing and stepping by keys lands on T only sometimes. Write one line, wait for
  `recorder: dump <name> armed for GameTime <T>` (the game deletes the file when it reads it), then
  the next; up to 8 at once. `… refused: the level is at GameTime <t>` means T has passed, and
  `… not taken: …` that a forward seek or the level's end passed it.
- **Play one**: `tacli replay play <i> FILE [--perspective RECORD]` launches the instance on the
  recording's map and seats and starts the skirmish; it exits 0 on `replay: playing <name>`.
- **Seek**: `tacli replay seek <i> T` moves the instance's running playback to GameTime T, forward
  or backward; `tacli replay seek <i> T --recording FILE` launches one that plays FILE from T.
  `--dump D` also takes a dump at T and prints its path. It waits for
  `replay: <name>: at GameTime <T>, sought from GameTime <h> in <ms> ms (read <ms>, rebuild <ms>)`
  or `replay: <name>: seek to <T> refused: <why>`, and exits 0 when it arrived, 1 when it was
  refused, timed out or nothing was playing, 2 on a usage error. A playback that has played to its
  end still seeks; a level that has ended does not (`replay: <name> at its level's end, …`). By
  hand, `seek <T>` or `seek <T> dump <name>` in the trigger does the same on a playing level;
  `dock seek <T>` asks it through the dock's own entry, as a click on its strip does (clamped to
  the recording, no dump: on a paused playback, arm `dump <name> at <T>` after the `sought from`
  line, which leaves it at T − 1).
  `replay play` and `replay seek --recording` take `--timeline N` to play a forked timeline; the
  playing crosses each fork with a seek (`replay: <name>: timeline <N> continues from GameTime <T>`).
- **The recorded camera** (a skirmish recording whose human seat's camera was recorded): `tacli replay follow <i> SEAT` is the cards' Follow — the camera shows that
  seat's, its zoom fitted to this screen, until the camera is moved (arrow keys, edge scroll,
  minimap) or the wheel turned; `tacli replay follow <i> free` is Free cam; `tacli replay jump
  <i> SEAT` puts the camera there once. Each waits for `replay: <name>: follow seat <S>, epoch
  <E>` (`jump to seat …`) or `… refused: <why>`. The end is logged as `replay: <name>: follow
  ended: camera moved|zoomed|free cam|…` (`jump ended: drawn` for a Jump). `tacli replay pov <i>`
  prints what the latest frame visit computed — `follow|jump epoch E seat S, tick n, sample …,
  view WxH, eye (x, y), level L/65536` — which `tools/replay/tarec.py pov FILE --eye n --viewer
  W,H` computes from the file (the eye before the camera's range clamp), then `; overlays: <n>
  items, <m> selected units`. Triggers: `follow <S>`, `follow free`, `jump <S>`, `pov`.
- **The overlays** (the same recordings; Vulkan only, GDI draws none): every recorded player's
  cursor, click rings and selection rectangles, in that player's colour. `tacli replay overlay
  <i> cursors|clicks|selections|chat on|off` is a switch of the dock's Overlays menu (the dock's
  pointer-arrow button, left of Leave); it waits for `replay: overlay <name> on|off`, or `replay:
  chat hidden|shown` for the chat. Every switch is on when a playing starts. Triggers: `overlay
  <name> on|off`, and `chat hide` / `chat show`.
- **Take over**: `tacli replay takeover <i> [FILE [--timeline N]] --at T --player P
  [--difficulty D] [--dump NAME --dump-at G]` makes the playback a live skirmish at T as player P,
  every other recorded seat an AI (D 0–2, by default the recording's difficulty, or 2 when it had
  no AI). With FILE it launches the instance on it, P viewed, and takes over once it plays;
  without, it takes over the running playback. It waits for `replay: <name>: taken over at
  GameTime <T> as player <P>, …` and the recorder's `recording timeline <I> of …`, and exits 1 on
  `… takeover at <T> refused: <why>`. The game after T is appended as timeline I to the file the
  level plays: the instance's copy, `gamedir/replays/<name>.imprec`, whose path tacli prints (`the
  fork:`), never FILE itself. `--dump` arms a dump of the live game at G before the takeover, so it
  is never late. By hand: `takeover <P> at <T>[ difficulty <D>]` in the trigger. The file must be
  writable and open in no other program.
- **Fork rules**: at most three live forks; a new one evicts the least recently used,
  excluding the selected playing and the original. Leaving a playing updates its last-used
  time when the file is writable. `tacli replay takeover <i> FILE --at T --player P --dry-run`
  predicts eviction without launching; without FILE it asks the current playing.
  `tacli replay forks <i> FILE` lists states and last-used times offline. Add `delete ID` or
  `export ID OUT` to mutate the running instance's actual `gamedir/replays` file, at any
  screen; OUT is a new simple stem there. Deleting the current playing is refused. Deleted
  ancestors remain available to descendants, and exported chains keep their timeline IDs.
  Triggers: `forks "NAME" delete ID`, `forks "NAME" export ID "OUT"`; wait for
  `replay: forks …: done|refused: …`. tacli matches its request token in the result;
  manual triggers need none. Every writer holds a persistent `.lock` file; never
  unlink one to bypass a busy file. Compaction is atomic on a worker and can be deferred
  while another reader holds the file; process exit waits for maintenance.
  `tareplay.py forks battle [--cover]` runs the dedicated checks. A fixture with `forks`
  also runs X after its existing lanes; battle and stock-limits exercise this on both builds.
- **Live rewind**: `tacli rewind <i> open|seek T|now|fork|close [--dump NAME]`
  operates on a running recorded skirmish. `open` pauses and reports the complete
  seek interval; `seek T` inspects iteration T at frame T−1; `now` restores the
  saved present; `fork` resumes from the inspected frame as a new timeline.
  `close` without a seek leaves the world intact, otherwise it returns to now.
  Each exit restores the pause state held before opening. Pause input cannot
  advance an open rewind. `--dump` writes a frame dump before the operation's
  success line and before unpausing. Wait for the CLI acknowledgement before
  sending another command. Once a fork ends the original, an append failure
  permits retrying `fork`, but cannot return to now.
  `tareplay.py rewind battle [--stock] [--cover]` runs the live rewind L/P/Q
  checks. Fixtures with `rewind` run the same checks in U; stock-limits uses
  the battle with the stock DLL. The `rewind_paused` scenario creates a human
  unit while paused before the saved-present dump, checking that return-to-now
  preserves the previous radar scan. Return-to-now reconstructs COB threads; the U
  AI check requires new planner-issued orders and a normal dispatch of sampled
  orders for each AI seat, plus the mobile-unit movement threshold. Construction
  is reported without requiring matching choices. The harness trigger
  `ai-activity NAME from START until END` observes that future half-open tick
  interval and reports per-seat orders/dispatches followed by its completion line.
  The shared automatic-targeting phase and restored orders do not count. The U
  rewind-specific count allowance reports VM START/CALL child-count differences
  via `compare-counts --rewind`. All count comparisons can report
  `accepted bounded COB flare` for the one COB explosion path whose accepted
  parent difference bounds a same-direction child difference; the completeness
  note specifies its limits. D2 excludes sight buffers only for unseated records.
  For the fresh comparison, the trigger
  `seek-frame T dump NAME` takes a paused frame dump in the seek's own visit.
- **Ending a game on purpose**: a `scenario apply` with `clear_existing: true` that leaves the
  human seat no unit is a defeat, and the level's end ends the recording, complete once the
  writer's `closed` line is in the log.
- The file format is `tagpu/ddraw/inc/tagpu_recfmt.h`; what is recorded and what is not:
  `research/notes/tadr-port/demo-recorder-skirmish-slice.md`.


## Resource snapping and Quick Build

The Impure options screen exposes `RESOURCE` (manual radius) and `QUICKBUILD` independently;
`PROFILE` shows the detected rules or a rejected-edit error. `arm menu.open` opens the gear
screen; `arm menu.open=off` closes it. This screen has no OK button. Effective controls are
locked when the profile forbids them or the resource hooks are unavailable.

Before a contextual Quick Build test, leave manual placement with Stop and wait for cursor
mode 1; Escape while a build queue is active is insufficient evidence of that transition.
Allow simulation to run while waiting for Stop to clear the order chain; while paused the
native button can change the cursor mode before its orders are processed.
Use the contextual button from Interface Type (zero left, nonzero right). Explicit commands
retain their native left-placement/right-cancel behavior. Input delivery is asynchronous:
assert a new order or the `resource: already queued` acknowledgment, not an unchanged old queue.

`tools/resource_fixture.py` prepares the resource fixtures' generated terrain and profiles in
an isolated stopped game folder. The `RS` maps are deliberately flat green. Stock geothermal
`CORGEO` is in `CORCK`'s menu; `ARMACK` does not build it. `tools/resource_regression.py` drives
live assertions through native input. The fixtures in `tools/replay/fixtures.json` run them
before replay comparisons; use `tools/replay/tareplay.py run resource-mex resource-upgrade` for
a focused run and `tools/replay/tareplay.py landing` for the landing gate. The mex assertion
compares manual and Quick placement at the same pointer and rejects a partial target at the
configured snap boundary.
Omit `--keep` for normal regression runs: the report removes passing frame dumps after its
checks, retaining failing dumps, logs, recordings and coverage. Peak space still includes the
whole run before reporting. Use `--keep` when passing dumps are needed for diagnosis, and clean
redundant artifacts only from completed runs; an active report still reads its collected dumps.
The classic fixture selects `LEFTCLICK` stage 0 in native Interface options after launch and
requires `main+0x37EFA == 0` before clicking. Setting the stopped instance's registry is
insufficient: every launch restores right-button orders. Other mex fixtures verify value 1.

For reclaim-to-Moho failures on Comet Catcher, use `tools/resource_upgrade_stress.py` in
an owned isolated instance. While stopped, `prepare <gamedir> /tmp/comet-upgrades.json
--phase 0 --sides mixed` creates private supply content and a scenario covering every
installed metal deposit. Load it with `tacli scenario load <name>
/tmp/comet-upgrades.json --defaults`, then `run <name> /tmp/comet-upgrades.json
--out /tmp/comet-upgrades-run`. Native mouse clicks issue the upgrades; the JSON report
distinguishes refusal, reclaim, nanoframe creation and completed construction. Use a
different phase or side for another sweep. Order inspection pauses the unit tick;
the final state is left paused for diagnosis. This test does not compare replays.
Generated constructor starts stay 128 world units inside the border; the report records
actual bearing/distance after clamping. Initial native placement refusals remain separate
from accepted upgrades that later stall or finish.

`resource-core-upgrade` is the completion replay regression on installed Comet Catcher.
Its private supply leaves CORACK, CORMEX and CORMOHO definitions intact. `check-core`
issues real contextual input and requires native reclaim/build, INBUILDSTANCE and progress;
`finish-core` requires a completed Moho and the old mex absent. The fixture's `last_min`
keeps its final comparison after that completion checkpoint on every commit seed.
Its `cob_legacy_save_probe.py --preflight <name> --gamedir <gamedir>` first exercises
native save/load with all ten records active on human and AI probes, using the
worker's held DLL. Paused game-thread peeks compare active records; the engine
writes `cover-resource-save.bin` on coverage builds. The recorder is disabled
through the preflight and restored before L starts.
Run `tools/replay/tareplay.py run resource-core-upgrade --cover` for the focused check.
That fixture's preflight enables native COB trace while recording is disabled, then restores
the lever before L starts. `stock-resource-mex` runs the same save/load preflight with
`--native-stock --trace` on eight 32-word records. Their coverage bitmaps exercise the allocator
observer in both builds; stack-peak observation belongs only to the guarded interpreter.
The trace and recorder counter both detour the allocator's pristine entry, so recorded lanes
keep tracing disabled and retain every ordinary sink counter.

The guarded COB runtime has ten 128-word slots; the stock-limits build has eight native
32-word slots. `cob_getter_probe.py --thread-exhaustion start|call` fills the guarded pool
on human and local-AI units. `cob_legacy_save_probe.py --out /tmp/cob-import` imports a
native save with all eight records active; `--expanded` saves/restores all ten guarded
records. Their archives and saved games stay private in the owned instance/output folder.

Resource preflights run native frontend checks before the L lane records a match.
`resource-frontend` tests raw radius preservation through Visuals Undo and profile cap reload.
`resource-refusals` refreshes `tacli units` before scenario validation so its cached catalogue
contains the private constructors; capability refusals must not issue action zero.
`resource-nozoom` uses `tagpu_zoom.off` and exercises stock-limits manual/Quick input with
pristine conversion. The fixture cleanup removes that lever before other launches.

The `resource-frontend` preflight disables packets while exercising Undo and cap reload, then
restores the lever before match recording. Profile polling follows the engine flip on every
renderer lane; a full mailbox or absent Vulkan consumer cannot stop menu reload.
