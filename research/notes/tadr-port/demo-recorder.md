# Demo recorder

## Purpose and current state

This is the living record of **demo recorder decisions, milestones, and high-level feature
progress** for Impure. The product decisions below were agreed in the design interview on
2026-09-28. **Implementation and performance are not established by this page.**

Keep this page at feature level: what is agreed, what works, what remains open, and a short link
to the evidence for a completed milestone. It is **not a detailed debug log or implementation
history**. Put investigations, disassembly, benchmark methods, failed experiments, and detailed
implementation history in separate evidence notes or git. Update the current status in place;
do not append a diary of attempts. Preserve explicit decisions and clearly distinguish proposed
techniques from accepted requirements.

The shared engineering rules are in [the TADR port overview](overview.md). Recording is a new
feature of Impure, drawing heavily on the Pascal recorder as prior art. It does not require
running the original recorder alongside Impure.

## Agreed scope

| Area | Decision |
|---|---|
| Games recorded | Multiplayer matches, including AI players in a multiplayer lobby. Ordinary offline skirmishes, campaigns, and games resumed from saves are outside the initial scope. |
| Participants | All players run Impure, which includes the recorder. Supporting unpatched multiplayer clients is not part of this work. |
| One-file replay | One participant's recording must be sufficient for solo playback, including the other players' recorded perspectives. No collection or merge of other participants' files is required. This covers the portion the recorder was connected to capture. |
| Watching | Solo, watch-only playback. Shared network viewing and taking control to continue a recorded game are outside this version. |
| Prior art | Reuse the Pascal recorder's knowledge heavily, verifying its assumptions and adapting it to Impure. The available unreleased source is not treated as the stable recorder players normally use. |
| Legacy recordings | Compatibility with old Pascal `.tad` recordings is optional, not a requirement. A new format and recording method are allowed; the exact format remains open. |

## Recording lifecycle and storage

- **Automatic recording is on by default**, with an opt-out setting. Start at match start and
  finalize on exit. Preserve a playable portion after a crash; the exact durability boundary
  remains to be designed and verified.
- **Continue after the recording player is defeated**, while they remain connected, through
  to the end of the match. If they leave or disconnect earlier, preserve the file, label it
  incomplete, and show its actual end time.
- **Keep recordings by default.** Show total replay storage in the browser. Offer optional
  cleanup by age or total size; favorites are protected from automatic deletion.
- **Larger files are acceptable when needed, but disk efficiency is a requirement.** Do not
  waste space or use size tolerance as permission for redundant data. Measure compression and
  the cost of seek support on representative matches.
- **Require matching installed map/mod content.** Record identities and fingerprints, check
  them before playback, and identify missing or mismatched content clearly. Do not bundle a
  copy of the map and mod into every replay.
- **Keep recordings playable across updates verified as compatible.** Store replay-format and
  Impure versions as well as content fingerprints. If an update changes playback incompatibly,
  require a matching playback version with a clear explanation. Never silently present a
  different match. How matching playback versions are obtained and retained is still open.

## Playback and seeking

| Feature | Agreed behavior |
|---|---|
| Entry point | **Replays is accessible from the main menu**, inside Impure's game interface. |
| Replay browser | List map, players, date, duration, completion status, and compatibility status. Include storage usage, favorites, and the optional cleanup controls above. |
| Controls | In-game timeline, pause/resume, and playback-speed controls. Exact speed choices remain open. |
| Forward seek | Fast and seamless, with a **target under one second**, including a jump such as minute 5 to minute 45. Hold the current frame until the destination is ready; do not visibly run through intermediate catch-up frames or restart the match for the viewer. This is an unmeasured target, not a delivered capability. |
| Backward seek | Included. A loading delay is acceptable initially; instant backward seeking is not promised. |
| Timeline events | Clickable commander deaths, player defeats/disconnects, and manually placed bookmarks. Seek to shortly before an event and offer to move the camera there. |
| Results | Show results and future event markers openly. **No spoiler filtering.** |
| Automation | Expose replay launch and seeking through `tacli` for automated verification. |

**Preparation is undecided.** A one-time indexing or checkpoint-generation step when first
opening a replay is not approved merely because it could make later seeks faster. It may be
rejected if it is slow or consumes too much space. Bring measured opening time, preparation
time, seek latency, replay size, extra cache size, and recording overhead before choosing.
There is no accepted preparation-time or cache-space budget, and no accepted exemption from
the forward-seek target based on an assumed preparation step.

## Player perspectives and overlays

- **Full battlefield visibility by default**, with a selector for each player's sight and
  radar. Player visibility must reflect what they could actually see at the selected time,
  including after seeking. The required capture/reconstruction data must be verified.
- **Free camera in either visibility mode.** Optional player-camera follow reproduces the
  recorded camera position and zoom. Panning or zooming manually returns to free camera.
- **Record every player's cursor movements, click indicators, selected units, and issued
  orders**, with independently switchable overlays. These join camera follow in the initial
  feature scope. One participant's file must contain the necessary contributions from the
  other participants; ordinary unit traffic alone is not assumed sufficient.
- These are recorded gameplay perspectives, not a promise to reproduce every menu, keystroke,
  or pixel of another player's interface. Full interface capture and selection-drag rectangles
  have not been agreed as separate features.

### Network and file efficiency

**Do not require a 60 Hz presentation-data stream.** Playback may interpolate camera and cursor
movement using both earlier and later recorded samples. Keep network traffic and disk usage
compact without losing discrete actions or changing their ordering.

Candidate techniques discussed in the interview are adaptive camera/cursor updates around
5–10 Hz while moving, no updates while unchanged, quantized coordinates, delta encoding,
selection additions/removals, and batched messages. Clicks, selections, and commands are
timestamped changes rather than interpolated actions. Mark camera jumps so a jump to a unit
does not become an invented camera sweep.

These are **directions to benchmark**, not a finalized wire specification, sampling rate, or
performance claim. Independently compressed file blocks with a time index are a candidate
for compact random access. Checkpoint frequency, representation, compression, and whether any
checkpoint data must cross the network are unresolved. Large files are not a substitute for
efficient encoding, and fast seeking must not imply unnecessary full-world network snapshots.

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
meaning across mods. Establish the recording data for player perspectives first; the statistics
dashboard can follow. Its exact delivery milestone is not yet fixed.

## Correctness requirement: false construction glow

Completed units must not appear to be under construction merely because they were created on
a remote peer or reconstructed during playback or a seek. Units genuinely under construction
must retain their correct build state.

There is relevant existing work: [simulation fixes, B8](sim-fixes.md) carries a fresh unit's
build fraction and HP in Impure's create message. `tagpu_patches.c` applies that state at remote
creation, and `tagpu_scenario.c` holds a scenario-created unit's messages until its final state
is set. B8's documented multiplayer measurements verified finished scenario units on three
peers. **That is prior evidence, not proof that recording or seeking is correct.** The note
also records unmeasured cases and a fallback without carried create state. Add replay-specific
verification, including rapid spawning, seeking, and genuinely unfinished units.

## Milestones and feature progress

These are tracking groups, **not a detailed implementation sequence or landing plan**. Reorder
or split them when the feasibility work supplies evidence. No demo-recorder implementation
milestone is marked complete by this design interview.

| Milestone | High-level completion criterion | Current state |
|---|---|---|
| Product contract | Scope, viewer features, storage policy, and explicit open decisions recorded here | **Decisions recorded, 2026-09-28** |
| Feasibility and format | Demonstrate capture/playback fidelity and compare seek approaches with measured time, space, and overhead; choose the format and preparation policy from those results | **In progress — [initial source/disassembly findings](demo-recorder-exploration.md); runtime measurements required** |
| Recording and basic playback | Automatic multiplayer capture, one-file solo playback, lifecycle/completeness handling, content/version checks, pause and speed control | **Planned** |
| Seeking and recovery | Forward-seek target measured, backward seek usable, playable crash-truncated files, correct restored state and visibility | **Planned** |
| Player perspectives | Full/player visibility, free/follow camera, cursor/click/selection/order overlays, and in-match communication | **Planned** |
| Replay browser and navigation | Main-menu entry, file metadata and results, storage/favorites/cleanup, timeline events and bookmarks, `tacli` launch/seek | **Planned** |
| Analysis | Per-player/team statistics at the playhead and over time, with documented metric definitions | **Planned** |
| Verification | Representative matches and mods, seek correctness, overhead/size measurements, and the construction-glow regression checked | **Required across milestones; not yet run for the recorder** |

For a completed milestone, add a short statement of delivered behavior and a link to its
evidence. Keep remaining gaps visible. Put detailed test runs and implementation history in
the linked material rather than growing this table into a debug log.

## Open technical decisions and evidence needed

1. **Capture and playback architecture.** Compare Pascal-style packet reconstruction with
   checkpoint-assisted approaches. Establish what a single participant can capture faithfully,
   and what other players must contribute for perspectives, communication, and statistics.
2. **Seeking state.** Verify the Pascal jump/resynchronization mechanism rather than assuming
   its file index is a complete world checkpoint. Impure's renderer frame packets likewise
   cannot be assumed to restore the simulation: some content is camera-limited and they are
   not full simulation saves.
3. **Format and compatibility.** Choose the schema, time basis, block/index layout, integrity
   and recovery rules, and compatibility policy for actual playback changes. A new format is
   allowed; a specific extension, codec, or snapshot scheme has not been chosen.
4. **Preparation tradeoff.** Report opening/preparation time, seek latency, replay MB per
   match-hour, cache size, recording overhead, and network traffic on representative matches.
   Include small and large battles and supported mods. Name the reference setup and workload;
   do not turn one measurement into a universal guarantee.
5. **Correctness and capacity.** Establish sufficient data for visibility, orders, effects,
   build state, statistics, and recorded local interactions at and after a seek. Define and
   test what happens at incomplete data or resource limits; do not silently claim completeness.

## BAR comparison and further candidates

Beyond All Reason is a feature reference, not proof that its replay architecture fits TA.
The comparison used BAR source revision `84ba89ae679c36a7e7f37fe2be12cc517f268d9f` and its
[replay guide](https://www.beyondallreason.info/faq/how-to-watch-replays).

| BAR reference | Impure decision |
|---|---|
| [Camera follow](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/camera_lockcamera.lua), [player cursors](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_ally_cursors.lua), [player selections](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_allySelectedUnits.lua) | Camera, cursor, and selection features agreed above; clicks and issued orders also included |
| [Order display](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_show_orders.lua), [command feedback](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_commands_fx.lua) | Issued-order overlays agreed; exact queue/overlay presentation remains a design detail |
| [Replay speed buttons](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_replaybuttons.lua) | Pause and speed controls agreed; BAR's exact speed presets are not a requirement |
| [Spectator HUD](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_spectator_hud.lua), [team statistics](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/gui_teamstats.lua), [APM](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luarules/gadgets/game_apm_broadcast.lua) | Player/team analysis agreed, with TA-specific definitions; BAR's two-team spectator-HUD restriction is not adopted as our requirement |
| [Automatic Player TV](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/84ba89ae679c36a7e7f37fe2be12cc517f268d9f/luaui/Widgets/camera_player_tv.lua) | Later candidate; no initial automatic-camera-switching commitment |
| [Public replay downloads](https://www.beyondallreason.info/replays) | One-file playback supports sharing recordings; a hosted replay service is separate and not in the agreed initial scope |

Automatic major-battle detection is also a later candidate. Fast seamless forward seeking,
backward seeking, crash recovery, and the construction-glow check are Impure requirements;
equivalent BAR capabilities were not established in the comparison.

## Prior-art pointers

- [TADR recorder survey](../tadr-merge-exploration.md#tplayxdll-the-recorder): the distinction
  between the shipped recorder and the unreleased source, and the recording/replay overview.
- `vendor/TADR/src/Recorder/idplay.pas`: capture and recording header creation.
- `vendor/TADR/src/Server/tasv.pas`: replay players, packet delivery, and seek/resynchronization.
- `vendor/TADR/src/Server/savefile.pas`: reading, file-offset indexing, and packet reconstruction.
- `vendor/TADR/src/Docs/saveformat.txt`: historical format reference; verify against the writer
  rather than treating the early format description as the current implementation.

The gitignored vendor checkout is a research input. Keep address-level findings and detailed
source verification in the engine map or a dedicated evidence note when implementation begins.
