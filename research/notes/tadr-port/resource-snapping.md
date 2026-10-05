# D. Resource snapping and mod profiles — design

**Design decisions agreed 2026-10-04; implementation pending.** This page specifies mex and
geothermal placement snapping, the mod profiles that permit it, and automatic profile reload.
The [quality-of-life inventory](quality-of-life.md) records what the supported mods' native TADR
builds ship. Those native features do not execute under Impure's [takeover](../compat/takeover.md).

## Scope and defaults

The first implementation covers mex and geothermal placement. Reclaim/wreck snapping, queue
editing, line/ring construction and other construction behavior controls retain their current
status in the inventory. Mod profiles initially govern resource snapping; other features can
adopt the profile mechanism in later work.

These are **proposed Impure defaults**, including the agreed Stock TA opt-in. They are not a
claim that Stock TA ships TADR or that Impure already snaps placement.

| Detected mod | Snapping permitted? | Initial player radius | Maximum radius |
|---|---|---|---|
| Stock TA | Yes | 3 | 3 |
| Total Mayhem | Yes | 3 | 3 |
| ProTA | Yes | 3 | 3 |
| TA Twilight | Yes | 3 | 3 |
| Escalation | No; locked off | 0 | 0 |
| TA Zero | No; locked off | 0 | 0 |
| Unidentified mod | No; locked off | 0 | 0 |

M/P/T's native defaults permit a radius of 3. Native E/Z disable availability, rather than
merely starting a player preference at zero. The inventory's
[activation evidence](quality-of-life.md#activation-evidence-for-older-conveniences) records the
old executable gates and later build configurations. Impure's profiles reproduce those mod
policies without relying on TADR to run.

## Mod rules and personal preferences

Impure supplies a `profiles/` folder with a general profile for each supported mod, plus an
unidentified-mod fallback. A mod may also have optional version-specific profiles. At startup,
create supplied files that are absent and preserve existing files, including edited files.
Optional version profiles are created only when supplied; discovering a new version does not
require generating an override.

Apply a general mod profile first. An exact matching version profile overrides only the values
it explicitly supplies; omitted values inherit the general profile. With no matching version
profile, use the general profile. A known mod with an unfamiliar version therefore retains its
mod policy. An unidentified mod retains the off fallback.

Version matching uses the verified identifier exposed by the running mod, with no version
ranges initially. A reported identifier may describe less than a complete package release:
Escalation GOLD 10.2.0's executable stores `10.2` in its version field, while the checked Mayhem
and ProTA loader settings supply `11.3.0` and `4.8`. An exact match to `10.2` does not establish
which patch release of that series is installed. Document the identifier used for each mod;
show the detected mod, version and selected profile in settings.

Personal settings stay in `impure.cfg`. Mex and geo share one player-controlled radius, from
zero to the active profile's maximum. Zero turns snapping off. Remember that preference per
mod, shared across versions of the mod. A profile restriction limits the effective radius
without overwriting the saved preference. A profile that denies snapping locks the player
control off; enabling it requires an edit to the mod profile.

The file schema, version-profile filename encoding and shared mod-identity interface are
implementation details to specify against the existing code. Profile paths must remain inside
`profiles/`; engine-supplied identifiers are data, not unrestricted path components.

## Automatic reload

Detect profile file changes automatically and apply validated changes in out-of-game menus. A
change made during a match takes effect after returning to the main menu. Pausing or opening
in-game options does not release the match's fixed policy.

Reload the complete selected general/version chain into a candidate policy, resolve inheritance,
and validate it before replacing the active policy. Publish a complete policy with a defined
ordering; readers must never observe mixed old/new fields. On failure, keep the current valid
rules, preserve the edited files and show an actionable error. Retrying a subsequent file change
uses the same validation path.

Startup needs a valid initial policy from the embedded mod defaults. File creation and loading
must report failures without overwriting an existing unreadable or malformed profile. The menu
must make a rejected file and the rules actually in effect distinguishable.

Future mod installation can request the same menu-boundary reload. Downloading and activating
mods is future work: profile reload alone does not establish that the engine can replace loaded
archives, definitions and scripts in the same process.

## Placement behavior

Use TADR's footprint-coverage rule for mexes. For each candidate placement within the square
search radius, count footprint cells whose metal value is strictly greater than the map's
`SurfaceMetal` background value. Prefer the highest count; ties favor the placement nearest the
raw mouse position. This scores qualifying coverage, not metal income or the sum of metal values.
If no candidate qualifies, retain normal placement.

The modern TADR caller also checks whether a selected mex target is stable under a further local
search before activating the override. Preserve the intended complete-target behavior; verify
its coordinate and footprint conventions before adopting the code-level test.

Occupied deposits remain snap targets for mexes. Their preview is red while ordinary engine
validation says construction is blocked. This supports preparing a reclaim-and-replace upgrade;
snapping does not grant permission to build over an occupied site.

Geo snapping finds the nearest placement accepted by the engine's buildability rules for the
selected geothermal building. Its resource recognition and yardmap reads must be bounded by
the actual definition/yardmap allocation. TADR's inspection of at most 64 yardmap bytes is a
source heuristic, not a complete identification guarantee for every mod footprint.

Holding Alt temporarily bypasses both snapping modes without changing the saved radius. The
preview and the resulting build order must use the same selected placement, including clicks
made without a preceding mouse move and modifier changes while the mouse stays still. Normal
engine validation and order dispatch remain authoritative.

### Metal maps

TADR's mex path has no separate metal-map classification. Its background threshold naturally
distinguishes qualifying patches from uniform surface metal.

| Metal distribution | Consequence of the coverage rule |
|---|---|
| Uniform cells equal to the map's background level | No qualifying candidates; normal placement |
| Background metal with richer discrete deposits | Deposits remain snap targets |
| A broad uniform region above the background level | Equal full-footprint scores favor the candidate nearest the mouse |
| Unequal richness values that are all above the threshold | Coverage determines the score; higher income is not guaranteed |

The uniform-region statement applies to candidates with the same qualifying coverage; map edges
and region boundaries can change their counts. It does not promise identical coordinates to
vanilla placement in every rounding case. Geo snapping remains independent of the metal threshold.
These consequences are derived from source; live metal-map verification is pending.
The [engine map](../exe-reverse-engineering.md#surface-metal-and-the-mex-snap-threshold) records
the stock property load and grid initialization that establish the background value's role.

### Exploration

Respect the player's terrain exploration rules. Previously discovered resources remain targets
under grey fog; resources hidden by unexplored black terrain do not influence snapping. Honor
the engine's fully revealed-map mode. Apply the rule to resource data used for scoring, so a
candidate straddling the exploration boundary cannot gain a score from unseen resource cells.

This is an agreed Impure rule. TADR's mex scorer itself reads the metal grid without an
exploration check. Its separate geo scorer delegates to engine buildability; that alone is not
evidence that all of our resource-data reads respect exploration.

## Source evidence and implementation work

The released [v2026.6.8 scorer](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/tahook.cpp#L175)
counts cells above the background threshold. Its
[selector](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/tahook.cpp#L292)
enumerates a square of candidate offsets and breaks maximum-count ties by pointer distance.
The [caller](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/tahook.cpp#L650)
contains the mex stability check and geo yardmap/buildability path. The same threshold/selection
rule was checked in the native Mayhem tag, v2024.3.31, and Escalation's v2026.8.6 tag; Escalation's
build configuration disables its availability.

Existing code provides starting points, not a verified snapping integration:

- `tagpu/ddraw/src/tagpu_settings.c` owns `impure.cfg`, closed value lists and personal-setting
  publication. The profile mechanism must preserve those ownership and test-control rules.
- `tagpu/ddraw/src/tagpu_replays.c` identifies supported mods from engine config/archive names
  and Twilight's main-menu label. Share a verified identity interface instead of duplicating
  mod tables. Validate the distinction between Stock TA and unidentified mods: the replay UI's
  retail placement fallback is not authorization to enable a gameplay convenience.
- `tagpu/ddraw/src/tagpu_mark.c`, `tagpu_packet_pub.c` and `tagpu_native.c` carry cursor/build
  preview data from the game thread to the renderer. Choose placement on the game thread and
  publish that choice; the renderer must not call engine placement functions or write engine
  input state.

Before implementation, establish the input/update and click hook contracts by disassembly,
including footprint parity, map boundaries, exploration lookup and the engine's buildability
entry point. Read map/definition objects under the engine's lifetime and validated bounds. A
pointer probe or a short interval between checks and reads is not a safety argument.

## Verification before landing

Run the feature in the game; compilation and source inspection do not verify placement. The
required behavior cases are:

- Stock TA and each supported mod, with fresh defaults and edited general/version profiles;
  profile locks, personal radius zero, inherited limits and saved preferences across versions.
- Normal deposits, occupied deposits with red previews, map edges and differing footprints;
  zoomed views, stationary-mouse Alt changes, immediate clicks and manually queued build orders.
- Uniform background-metal maps, above-background uniform regions and mixed deposit maps;
  coverage ties and values demonstrating that the rule does not maximize income.
- Unexplored terrain, discovered terrain under grey fog and fully revealed maps; scoring across
  exploration boundaries without reading unseen resources into the result.
- Geothermal sites with valid and blocked placements, including supported mod yardmaps.
- Automatic reload of valid edits, inherited version overrides, rejected edits and later repair;
  menu application, match deferral and a coherent policy seen by every consumer.

Any simulation state or engine effect introduced by the chosen integration follows the existing
[recorder completeness rules](demo-recorder-completeness.md). Determine the applicable state
declarations and replay fixtures from that integration; do not assume that a local convenience
can bypass the simulation landing gates.

## Future work

During multiplayer, compare players' mod-level feature on/off policies for parity. Once in-game,
report a mismatch in chat with the player's name and issue, using the form
`WARN PlayerName: issue`. This is a TODO, not part of the first snapping implementation. Peer
capability discovery, transport, comparison fields and aggregation of several issues still need
a design.

Downloading mods and activating them without restarting TA is also future work. Investigate the
engine's archive, definition and script lifetimes before defining an activation mechanism. The
agreed reload boundary is the menu, with requests deferred throughout a match.
