# Demo recorder

## Purpose and current state

This is the living record of **demo recorder decisions, milestones, and high-level feature
progress** for Impure. The product decisions below were agreed in design interviews on
2026-09-28; the second replaced the packet-based design with state recording and added
rewind, takeover and forks. **Implementation and performance are not established by this page.**

Keep this page at feature level: what is agreed, what works, what remains open, and a short link
to the evidence for a completed milestone. It is **not a detailed debug log or implementation
history**. Put investigations, disassembly, benchmark methods, failed experiments, and detailed
implementation history in separate evidence notes or git. Update the current status in place;
do not append a diary of attempts. Preserve explicit decisions and clearly distinguish proposed
techniques from accepted requirements.

The shared engineering rules are in [the TADR port overview](overview.md). Recording is a new
feature of Impure; it does not require running the original recorder alongside Impure.

**No production recorder has landed.** The next work is the three feasibility experiments under
[Milestones](#milestones-and-delivery-order). Every decision of the
[completeness design](demo-recorder-completeness.md#decisions-taken) is taken. The [state design evidence](demo-recorder-state-evidence.md)
holds the desk research behind this design: how the engine treats remote players, what a rebuild
must carry, per-player state, and the size estimate. The [packet-era exploration](demo-recorder-exploration.md)
holds the earlier design's experiments and lists which of its findings still apply.

## Agreed scope

| Area | Decision |
|---|---|
| Games recorded | **Skirmish and multiplayer games**, including AI players in a multiplayer lobby. A skirmish loaded from a savegame is recorded from the moment it loads and has no history before it. **Campaign missions are out:** their scripted objectives and triggers are not unit state, and a campaign plays under its map's own unit limit. Recording campaigns to watch only would be a separate feature. |
| Participants | In multiplayer, all players run Impure, which includes the recorder. Supporting unpatched clients is not part of this work. |
| Recording method | **State, not packets.** A recording holds the recorded game state (unit state, per-player state and discrete events) and never depends on re-running the simulation. Skirmish and multiplayer recordings share one format and differ only in their metadata and player names. |
| One-file replay | One recording is sufficient for playback, including the other players' recorded perspectives in multiplayer. No collection or merge of other participants' files is required. |
| Watching | Solo playback in **puppet mode** (below). Shared network viewing is outside this version. |
| Taking over | Any recording can become a playable game at any recorded moment: **Play from here** rebuilds a simulation from the recorded state and starts a new fork. Fidelity is close, not exact. |
| Legacy recordings | Old Pascal `.tad` recordings are **not supported**: playing them would need the packet playback path this design leaves behind. |
| Prior art | The Pascal recorder remains a reference for message semantics and for its chat and camera records. Beyond All Reason is a feature reference (below). |

## How it works

These are the agreed architecture decisions. Establishing that they are feasible is the first
milestone.

- **Recorded state.** Per-unit state: type, owner, incarnation, position, heading, HP, build
  state, stance, kills, and the order queue, which also holds a factory's queued builds.
  Per-player state: metal and energy, camera, cursor, clicks and selection, and the explored map
  when the game's mapping option is on. Discrete events: shots, deaths, script starts, sounds and
  chat. Continuous values are recorded when they change and interpolated during playback from
  both earlier and later samples, so playback can be smoother than the recorded rate. Discrete
  events are never interpolated.
- **Sight is recomputed, not recorded.** In puppet mode the engine rebuilds each player's line of
  sight from the recorded unit positions, as it does for another player's units in a network
  game, and computes radar and sonar for the player whose perspective is selected. The explored
  map is history rather than current state, so it is stored in snapshots and restored at every
  seek and takeover, **only while the mapping option is on**: a game played with the map
  explored, which the owner reports is how multiplayer is usually played, stores no exploration
  at all. The sight mode is
  recorded as it changes, since chat commands can switch it during a match, and separately from
  the whole-map reveal a defeated player's own machine switches to.
- **Puppet mode for watching.** A real engine level is loaded and every recorded player is
  handled like a remote peer, so the engine does not simulate their units. Each tick Impure
  writes the recorded state into the units and triggers the recorded events; the engine animates,
  draws effects and plays sounds itself, as it does for another player's units in a network game.
  The engine's own interface (unit information, the resource bar, the minimap, fog, the order
  markers) shows the recorded values. Animations and effects are close to the original, not
  pixel-exact.
  - **The recorder is the only writer** of the state it records: the units' own scripts still
    run, but in puppet mode they change no engine state (their activation, flags and transport
    links come from the recording).
  - **Owner-only presentation shows as the recording machine showed it.** Mods such as
    Escalation draw indicators only for units their machine simulates (COB getter 75); a puppet
    answers that question as the recording machine did, per player. In a multiplayer recording
    that is the recording player's view: other players' own indicators were never on that machine.
  - **Effects only the owner's machine produces**, such as the nano spray, are recorded where the
    engine produced them, tied to their unit, and replayed through the same engine function.
- **Rebuild for seeking and takeover.** A seek reconstructs the state at the target from the
  nearest full snapshot and the recorded changes, then rebuilds the puppet world in place. A
  takeover performs the same rebuild under local ownership: **the seats become local first, and
  the units are then re-created**, because only a create gives a unit fresh scripts and a mod's
  scripts test ownership as they start. The rebuild carries units with their state, order
  queues and factory queues, player resources and sight. **Dropped at a takeover:** shots in
  flight, unit script state (so what a mod keeps in its scripts, such as an upgraded look,
  resets [INFERRED from the scripts]) and AI internal state; weapon reload and targets, each unit's current
  movement path (units re-path from their orders) and the AI's squad state beyond each unit's
  membership (which an engine call can set) are also lost unless each is given its own writer. The [scenario applier](../scenario-format.md) is the existing rebuild
  machinery, and the stock AI already adopts units the applier creates for an AI seat (measured:
  a scenario-spawned AI commander walked off and built). Its gaps: full-precision positions,
  turns, HP and build state (it writes whole units, whole degrees of heading only and 1 % steps);
  the unit's layer, since every create passes the ground layer; a complete
  clear (it misses the last slot and leaves features and wrecks); queues beyond a unit's first
  order; factory queues; resources (a write made during play does not stick: the 30-tick economy
  pass re-sums storage and clamps current to it, and one measured case is still unexplained);
  transport links; recorded slot indices; and turning a puppet seat into a working AI without
  reloading the level.
- **Snapshots cost the game thread nothing.** The writer thread keeps its own copy of the world,
  updated from the change stream, and writes full snapshots from that copy. This relies on the
  change stream being complete, which playback requires anyway.

## Timelines and forks

- A recording is a **tree of timelines**. **Only Play from here creates a fork**; seeking,
  watching and pausing never do.
- A fork is self-contained after its fork point: the rebuilt world is its first full snapshot,
  and it needs its parent's history only to watch the time before the fork. Deleting a timeline
  removes only what no remaining timeline needs; shared history is never lost.
- **The recording's original timeline is never deleted automatically**, and it can have **up to
  three forks**. Creating a fourth deletes the fork **least recently played or watched**, never
  the one in use. Skirmish and multiplayer recordings follow the same rule, so a multiplayer
  match survives any number of forks.
- There is no manual "keep" for a single fork in the first version; favoriting protects the
  whole recording from cleanup.

## Live rewind (skirmish)

1. **Open the timeline** with a hotkey, an in-game menu button, or from the victory/defeat
   screen. The game pauses.
2. **Scrub.** The world switches to puppet mode and shows the past, with the normal replay
   controls, overlays and perspectives.
3. **Leave** by **Play from here**, which takes over at the playhead and creates a fork, or by
   **Back to now**, which returns to the present without creating a fork.

Puppet mode overwrites the live world, so **Back to now is itself a rebuild**, with takeover
fidelity. Opening and closing the timeline without moving the playhead costs nothing.
**There is no timeline during a live multiplayer game**, because that world is shared with the
other players; a multiplayer game is rewound from its finished recording.

## Taking over

- **In a live skirmish rewind** the player stays themselves, and AI players stay AI at their
  original difficulty.
- **From a recording**, a takeover dialog picks **one player still alive at that moment**; every
  other player becomes a stock AI. **All AIs share one difficulty**, as in TA's own skirmish menu,
  which the engine holds as a single value. The dialog offers it, defaulting to the recording's
  own difficulty, or to the hardest when the recording had no AI players.
- Teams, alliances, resource and radar sharing, colours and names are kept, and converted players
  are marked as AI. The fork is a single-player game, and its metadata says so.
- **The AI starts fresh from the world.** It inherits the player's units, resources and recorded
  order and factory queues, so current jobs continue until it decides otherwise. Behaviour shifts
  at the takeover point.
- **An unfinished recording** (a crash, or leaving early) opens in the Replays browser like any
  other: fork from any point or play on from its end. There is no separate "Continue" feature.
- **More than four players.** A stock skirmish seats four players (measured: it refused every
  unit of seats 4–9, [scenario format](../scenario-format.md)), so hosting a larger recording in
  puppet mode, and forking it into a single-player game, depend on the first feasibility
  experiment seating up to ten players without a network session.
- **Not in the first version:** resuming with more than one human.

## Recording lifecycle and storage

- **Automatic recording is on by default** in skirmish and multiplayer, with an opt-out. In a
  skirmish the opt-out also disables rewind, which reads the recording. In multiplayer it means
  only "do not save my file": the player still sends what the other players' recordings need
  ([below](#multiplayer-contributions)).
- **Continue after the recording player is defeated**, while they remain connected, through to
  the end of the match. If they leave or disconnect earlier, preserve the file, label it
  incomplete, and show its actual end time.
- **One file per recording**, holding the original timeline and its forks, which are appended to
  it. Copying the file shares all of it. **Export timeline** writes a standalone file holding one
  timeline's history, which is how to share only the real match. A deleted fork leaves dead space
  that is reclaimed by rewriting the file when the recording is closed; the new copy replaces the
  old one only once it is complete. The original timeline's blocks are copied unchanged, never
  re-encoded or modified.
- **Keep recordings by default.** Show total replay storage in the browser. Offer optional
  cleanup by age or total size; favorites are protected from automatic deletion.
- **Disk budget.** Targets per match-hour: **at most 10 MB for a 1v1, 25 MB for a
  typical four-player game, and 100 MB for ten players with 1,500 units.** Moving units are
  recorded ten times a second in whole world units with 8-bit headings and interpolated during
  playback; snapshots, which takeover uses, keep full precision. Precision rises only if the
  fidelity check shows visible error. These targets rest on an estimate, not a measurement
  ([open decisions](#open-decisions-and-evidence-needed)); a real hour-long four-player game with AI and structures is measured before
  the format is frozen, and if the targets cannot be met the numbers come back for a decision.
- **Preserve a playable portion after a crash.** The exact durability boundary remains to be
  designed and verified; a process crash and a power loss are separate promises.
- **Recording cost during live play:** at most **0.5 ms per simulation tick on average and 1 ms
  at worst** on the game thread with 1,500 units, measured with the recorder off and on.
  Compression, disk writes and multiplayer sends run off the game thread. **The game never waits
  for the recorder:** if the writer falls behind, the disk fills or a write fails, recording
  stops at the last complete block, marks itself incomplete and says so in one line. The
  recorder's memory has a fixed ceiling; about 64 MB is the starting point, to be confirmed by
  measurement.
- **Require matching installed map/mod content.** Record identities and fingerprints, check them
  before playback, and identify missing or mismatched content clearly. Do not bundle a copy of
  the map and mod into every replay.
- **Keep recordings playable across updates verified as compatible.** Store replay-format and
  Impure versions as well as content fingerprints. If an update changes playback incompatibly,
  require a matching playback version with a clear explanation. Never silently present a
  different match. How matching playback versions are obtained and retained is still open.

## Playback and seeking

| Feature | Agreed behavior |
|---|---|
| Entry point | **Replays is accessible from the main menu**, inside Impure's game interface. |
| Replay browser | One row per recording: map, players, date, duration, completion, compatibility and fork count, with storage usage, favorites and the cleanup controls above. Expanding a recording lists its timelines with fork point, player played, length, outcome and last played; a fork can be deleted by hand. Forks are named from their point and player, for example "Fork 1, from 25:00, as ARM player"; renaming is not in the first version. |
| Timeline bar | The whole history of the current timeline, with fork points as markers and a selector to switch timelines. Switching is instant before the fork point, where history is shared, and an ordinary seek after it. |
| Controls | Pause/resume and playback-speed controls. Exact speed choices remain open. |
| Seeking | **Under one second in either direction, over any distance.** The current frame is held until the destination is complete; no intermediate frames are shown. Dragging the playhead shows the target time and the world rebuilds on release; continuous live scrubbing is not a target. |
| Takeover | **Play from here** and **Back to now** complete **in under two seconds** with a brief "Resuming…" indicator, without a loading screen or a level reload. Play from here asks for confirmation only when it would delete a fork, and names that fork. |
| Opening | Opening a replay loads the map and content as starting a game does; the normal loading screen is acceptable. |
| Preparation | **None.** The recording carries its own snapshots, so opening needs no indexing pass. |
| Timeline events | Clickable commander deaths, player defeats/disconnects, manually placed bookmarks and fork points. Seek to shortly before an event and offer to move the camera there. |
| Results | Show results and future event markers openly. **No spoiler filtering.** |
| Automation | Expose replay launch, seeking, takeover and forks through `tacli` for automated verification. |

The seek and takeover times are **unmeasured targets**. If a rebuild at 1,500 units cannot meet
them, the measurements come back for a decision; the targets are never relaxed silently. The
snapshot interval is chosen by measurement to meet them.

## Player perspectives and overlays

- **Full battlefield visibility by default**, with a selector for each player's sight and radar.
  A player's view shows **what the engine computes from that player's recorded units** at the
  selected time, including after seeking. That is close to what they saw, not guaranteed
  identical: another machine's copy of a player's sight has been measured differing in 34 cells
  for a single moved commander while the unit positions agreed
  ([§6c](demo-recorder-exploration.md#6c-remote-perspectives-require-authoritative-contributions)).
- **Free camera in either visibility mode.** Optional player-camera follow reproduces the recorded
  camera position and zoom. Panning or zooming manually returns to free camera.
- **Every player's cursor movements, click indicators, selected units and issued orders**, with
  independently switchable overlays. **Queued movement orders** appear through the engine's own
  order markers, fed from the recorded queues. **A factory's queue** is shown for a selected
  factory; whether through the engine's build menu or an overlay of ours is a design detail.
- **Sound:** replays play the engine's event sounds as a live game does, under the normal volume
  settings.
- These are recorded gameplay perspectives, not a promise to reproduce every menu, keystroke, or
  pixel of another player's interface. Full interface capture and selection-drag rectangles have
  not been agreed as separate features.

## Multiplayer contributions

A player's machine does not hold the other players' order and factory queues, their actual
resources, or their camera, cursor and selection: TA replicates unit state, not orders, and
[measured remote copies](demo-recorder-exploration.md#6c-remote-perspectives-require-authoritative-contributions)
of production read zero. Each machine therefore sends these, with chat and the statistics
counters, for its own players and for the AIs it hosts. Sight is not sent: playback recomputes
it (above).

- **Every Impure player sends contributions in every multiplayer game**, whether or not they
  record. There is no option to be left out of other players' recordings.
- **Continuous data** (camera, cursor) is sent only when it changes, a few times a second, and a
  lost sample is acceptable. **Discrete data** (orders, queue changes, selections, clicks, chat)
  is delivered reliably and in order.
- **Contributions never delay game traffic.** Their bandwidth budget is set by measurement
  against TA's own traffic. An arithmetic estimate, not a measurement, puts camera and cursor
  alone at about 1.5–3 KB/s outgoing per player in a ten-player game, at 5–10 samples a second.
- A player who leaves stops contributing, and recordings mark that player's perspective as ending
  there.

## Communication

- Include **all in-match public chat, team chat, private whispers, and map pings/drawings**.
- **Do not record lobby chat.**
- Tie communication visibility to the replay perspective: full-map mode can show all teams;
  a selected player's perspective shows what that player received.
- Provide a hide-chat display toggle. Private whispers are included in the recording, not
  omitted as an exclusion policy.

## Statistics

Provide **per-player and per-team comparisons**, both values at the selected replay time and
graphs over the match:

- metal and energy income, spending, and storage;
- army value;
- units built and lost;
- kills;
- actions per minute (APM).

Document each metric's definition, especially army value and APM, so comparisons have a clear
meaning across mods. The statistics dashboard follows the skirmish and multiplayer slices.

## Completeness and verification

Impure keeps adding simulation behaviour: fixes that carry new state in its own messages, new
per-unit data, spare engine bytes put to new use. A recorder that silently misses any of it
produces replays and takeovers that are wrong without anyone noticing. **The recorder must stay
complete as the simulation changes, and a gap must be loud and named, never silent.** It must not
be brittle either: no hand-maintained list of fields that goes stale with every feature, and no
check that raises false alarms on ordinary changes.

- **Continuous self-verification, always on, in every recording.** At each tick boundary the game
  thread copies a small rotating slice of the real game state, within the recording-cost budget,
  and the writer thread compares it with the recorder's own copy of the world, built only from
  what it has recorded; the writer never reads engine memory itself. At 50 units a tick, 1,500
  units are all checked within 30 ticks, about half a second to a second depending on game
  speed. Every byte is compared except those classified once as deliberately not recorded
  (pointers, caches, values the engine recomputes, internal timers). The unit and player record
  layouts are fixed by the retail executable, so that classification is stable, and a new use of a spare byte is caught by the
  first check that sees it written; a value written and cleared between two checks is not. On a mismatch in a player's game, the true values go into
  the recording as a correction, so the replay is wrong for at most that rotation period; the
  file's metadata names the field and counts the corrections,
  and one line is logged per field. In the test harness any mismatch fails the test.
- **An automated replay test harness** runs many replay scenarios in parallel, the way the
  compatibility suite runs games, from small skirmishes to ten players with thousands of units,
  with AI, mods and multiplayer. Its central test is the **restore round trip**: during a scripted
  game, restore the world in place at the current moment from the recording and compare the whole
  state after with the state just before — every unit, player and feature record and Impure's
  own tables — against the explicit list of what a takeover deliberately drops. Anything else that
  is missing or different is a named failure. It also compares puppet playback with the recording,
  checks that seeking to one moment from different starting points gives the same world, and
  compares event counts taken where the engine creates effects, sounds and script starts, which
  catches events that leave no state behind. **A landing that changes simulation code runs the
  fixtures of the features it changes**, each recorded, restored in place and replayed; a change
  whose feature names no fixture fails the landing by name. The broad tier runs nightly on
  `main` and before every release tag.
- **Impure's own state is declared where it is defined** (option B of the
  [completeness design](demo-recorder-completeness.md), chosen 2026-09-29). Engine unit and player records are recorded whole by default, apart from
  one exclusion file the owner controls, which the build hash-checks. These rules go into the
  project's CLAUDE.md with the mechanism, in the landing that brings the declarations and the
  build check. In a source file marked as
  simulation code, every table, static variable and thread-local slot carries a one-line
  declaration next to its definition: its class (recorded state; content rebuilt from game data
  and hashed; transient, empty at every snapshot; or private diagnostics) and what a row index
  means (unit slot, unit type, player, weapon). The declarations land in one linker section that
  the recorder walks, so a declared table is recorded, checked and compared with no code of its
  own. Heap tables come from one allocation wrapper. Tables store IDs, not addresses, or declare
  which bytes are IDs. A build check refuses an undeclared table or variable in a simulation file
  and names it. State Impure keeps inside engine-allocated objects needs a layout declaration,
  which only the harness can catch missing.
- **Event counts** are taken where events start (creates, deaths, shots, script starts) and where
  their effects are produced (explosions, particles, sounds), and compared by the harness. New
  effects start through an engine entry point, never a direct call to where effects are produced.
  An effect only the owner's machine produces is recorded at its source and tied to its unit by an
  engine invariant, never imitated; an emission the recorder cannot tie is a named failure.
- **A replay shows what the game showed.** Display state that follows from simulation events is
  recorded whenever the live game displayed it, for example the part of a unit's minimap blink
  that `nomapweaponalert` keeps quiet for harmless hits (`s_quiet`). The private class is only for
  diagnostics nobody sees.
- **Extra weapons is always on and fails closed**, like every other simulation feature, so a
  recording carries no arming dimension and tests run the same simulation as players. Its on/off
  switch and its silent disarm go; that is a small landing of its own.

## Correctness requirement: false construction glow

Completed units must not appear to be under construction merely because they were rebuilt at a
seek or a takeover, or driven in puppet mode. Units genuinely under construction must keep their
correct build state.

Existing work: [simulation fixes, B8](sim-fixes.md) carries a fresh unit's build fraction and HP
in Impure's create message, and `tagpu_scenario.c` holds a scenario-created unit's messages until
its final state is set. The packet-era prototypes checked
[rapid finished-unit spawning](demo-recorder-exploration.md#6a-first-solo-engine-playback) and
[a genuinely constructed solar](demo-recorder-exploration.md#genuine-construction-and-scene-switching)
on their own backends. That is prior evidence, not proof for the state design, which needs its
own checks: rapid spawning, rebuilds at a seek and at a takeover, and genuinely unfinished units.

## Milestones and delivery order

**Skirmish first, multiplayer second.** The skirmish slice needs no network work yet exercises
every hard part: state capture, puppet mode, rebuilding in place, forks and the file format.
Multiplayer adds contributions on top of a state pipeline that already works.

| Milestone | High-level completion criterion | Current state |
|---|---|---|
| Product contract | Scope, architecture, viewer features, storage policy and open decisions recorded here | **Decisions recorded, 2026-09-28** |
| Feasibility | Three experiments, most decisive first: (1) a level holding remote-type players driven by Impure without a network session: seating up to ten players, which a stock skirmish refuses beyond four (measured, [scenario format](../scenario-format.md)), and a viewer with no seat of its own, since a ten-player recording fills every seat; (2) rebuilding 1,500 units in place with their state and order queues, timed against the seek and takeover targets, and turning a puppet seat into a working AI without a level reload, by making the seat local and re-creating its units; (3) recording a real skirmish: how close the puppet replay is to the original, size per hour, and game-thread cost | **Next.** Desk research in the [state design evidence](demo-recorder-state-evidence.md); packet-era experiments in the [exploration](demo-recorder-exploration.md) |
| Skirmish slice | Recording, puppet watching, seeking, live rewind and takeover, forks, the Replays browser, `tacli` | **Planned** |
| Multiplayer slice | Contributions, the other players' perspectives, chat and pings, multiplayer takeover with the others as AI | **Planned** |
| Analysis and polish | Statistics dashboard, bookmarks, speed presets | **Planned** |
| Verification | The automated parallel harness and its restore round trip, built with the skirmish slice and extended by each later one; continuous self-verification in every recording; seek and takeover latency; size and live-cost measurements; the construction-glow checks | **Required across milestones; not yet built** |

If the puppet-host experiment fails, the watch path comes back for a decision before either slice
is built; the shelved packet playback and renderer-scene playback are the only known alternatives. If a takeover cannot build its AI seats without reloading the level, the fallback is a
takeover behind a loading screen, and that comes back for a decision too. For a completed milestone, add a short statement of delivered behavior and a link to its
evidence, and keep remaining gaps visible.

## Open decisions and evidence needed

1. **The completeness mechanism's open items.** Every decision is taken
   ([decisions taken](demo-recorder-completeness.md#decisions-taken)); three
   [open items](demo-recorder-completeness.md#open-items) remain for the landings that build it: a
   home for install-time plumbing, whether a recorded activation plays a second sound, and applying
   a recorded yard change.
2. **Measured size.** The disk budget rests on an estimate built from measured per-unit costs:
   about 2.4–3.3 compressed bytes per changed unit per sample, about 10–11 bytes per unit in a
   full-precision snapshot. Its largest unknown is the fraction of units that change per sample over a real
   hour; the only moving samples are two ten-second battles. With sight recomputed rather than
   recorded, the [estimate](demo-recorder-state-evidence.md#4-size-estimate) is about 8–36 MB per
   hour for four players with 500 units and about 24–108 MB for ten players with 1,500, so the
   four- and ten-player budgets sit inside the estimated ranges rather than safely below them. The
   1v1 target has only a static-battle estimate, about 3.5–6.5 MB per hour (up to 9.4 with the
   explored map stored), and a moving 1v1 is not estimated. Recording at 5 Hz
   instead of 10 Hz saves only about 38 % of the unit-state stream, about a fifth of the whole
   estimate, so the rate is chosen for smoothness.
3. **Snapshot interval**, chosen by measurement against the seek target and the disk budget.
4. **Playback speed presets.**
5. **Crash durability boundary**, and its wording for a process crash versus a power loss.
6. **Contribution protocol**: the message envelope (versioned and length-bounded, but not
   negotiated: the overview's rule 1 assumes one build and no handshake), the bandwidth budget and
   the measured wire cost.
7. **Other players' units in a multiplayer recording.** A player's machine holds other players'
   units as replicated copies. Their HP and build progress lag the owner's until TA's rotation
   refreshes them, about once every 50 seconds under a 1,500-unit limit. Kill counts are counted
   by each machine at every death, can differ from the owner's for good, and nothing replicates
   them.
   Whether owners also send corrections for their own units, and at what bandwidth, is decided in
   the multiplayer slice with measurements.
8. **Metric definitions** for army value and APM; the
   [exploration](demo-recorder-exploration.md#visibility-communication-and-analysis) holds a
   recommendation.
9. **Matching playback versions**: how a player obtains and keeps a version that plays an
   incompatible recording.

## BAR comparison and further candidates

Beyond All Reason is a feature reference, not proof that its replay architecture fits TA.
The comparison used BAR source revision `84ba89ae679c36a7e7f37fe2be12cc517f268d9f` and its
[replay guide](https://www.beyondallreason.info/faq/how-to-watch-replays).

| BAR reference | Impure decision |
|---|---|
| [Camera follow](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/camera_lockcamera.lua), [player cursors](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_ally_cursors.lua), [player selections](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_allySelectedUnits.lua) | Camera, cursor, and selection features agreed above; clicks and issued orders also included |
| [Order display](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_show_orders.lua), [command feedback](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_commands_fx.lua) | Issued-order overlays agreed; queued movement orders through the engine's own order markers, and a selected factory's queue through the build menu or an overlay (a design detail) |
| [Replay speed buttons](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_replaybuttons.lua) | Pause and speed controls agreed; BAR's exact speed presets are not a requirement |
| [Spectator HUD](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_spectator_hud.lua), [team statistics](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_teamstats.lua), [APM](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luarules/gadgets/game_apm_broadcast.lua) | Player/team analysis agreed, with TA-specific definitions; BAR's two-team spectator-HUD restriction is not adopted as our requirement |
| [Automatic Player TV](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/camera_player_tv.lua) | Later candidate; no initial automatic-camera-switching commitment |
| [Public replay downloads](https://www.beyondallreason.info/replays) | One-file playback supports sharing recordings; a hosted replay service is separate and not in the agreed initial scope |

Automatic major-battle detection is also a later candidate. Fast seeking in both directions,
rewind and takeover, forks, crash recovery and the construction-glow check are Impure
requirements; equivalent BAR capabilities were not established in the comparison.

## Prior-art pointers

- [Scenario format](../scenario-format.md) and `tagpu_scenario.c`: runtime creation of units,
  features, orders and resources through the engine's own calls, the rebuild machinery to extend.
- `tagpu_order.c`: the port of the engine's order markers, which walks each unit's main order
  list (`+0x5C`); whether a factory's queued builds sit there or on the sub list `+0x60` is
  untested.
- [Engine map](../exe-reverse-engineering.md): the player controller byte distinguishes local
  and remote players, and a remote player's units move through proxies fed by incoming state,
  the mechanism puppet mode builds on.
- [Packet-era exploration](demo-recorder-exploration.md): the file container and process-crash
  recovery, owner authority for resources, the measured sight difference that recomputation
  accepts, and the rule to hold the old image until the new one is ready.
- [TADR recorder survey](../tadr-merge-exploration.md#tplayxdll-the-recorder) and the gitignored
  `vendor/TADR/src/Recorder/idplay.pas`: the Pascal recorder's chat forwarding and camera records.
