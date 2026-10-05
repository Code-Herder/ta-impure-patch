# D. Resource snapping, Quick Build and mod profiles — design

**Design decisions agreed 2026-10-04; implementation pending.** This page specifies mex and
geothermal placement snapping, contextual Quick Build, the mod profiles that permit them, and
automatic profile reload. Quick Build follows the BAR resource-building workflow: select a
capable constructor, hover a deposit or geothermal vent, see the building ghost, and use the
configured command mouse button to build without choosing a build-menu icon.
The [quality-of-life inventory](quality-of-life.md) records what the supported mods' native TADR
builds ship. Those native features do not execute under Impure's [takeover](../compat/takeover.md).

## Scope and defaults

The first implementation covers manual mex/geo placement snapping and a separately controlled
Quick Build mode for mexes and geothermal plants, including owned-building upgrades. Reclaim/wreck
snapping, dragging queued orders, line/ring construction and other construction behavior controls
retain their current status in the inventory. Mod profiles initially govern these resource
features; other features can adopt the profile mechanism in later work.

These are **proposed Impure defaults**, including the agreed Stock TA opt-in. They are not a
claim that Stock TA ships TADR or that Impure already implements either resource feature. Quick
Build is an additional Impure design, separate from the inventory's native TADR snapping row.

| Detected mod | Manual snapping permitted? | Quick Build permitted / initial player toggle | Profile default radius | Maximum radius |
|---|---|---|---|---|
| Stock TA / OTA | Yes | Yes / on | 3 | 3 |
| Total Mayhem | Yes | Yes / on | 3 | 3 |
| ProTA | Yes | Yes / on | 3 | 3 |
| TA Twilight | Yes | Yes / on | 3 | 3 |
| Escalation | No; locked off | No; locked off | 0 | 0 |
| TA Zero | No; locked off | No; locked off | 0 | 0 |
| Unidentified mod | No; locked off | No; locked off | 0 | 0 |

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
zero to the active profile's maximum. Initialize a missing preference from the profile default.
Zero turns manual snapping off. Remember that preference per
mod, shared across versions of the mod. A profile restriction limits the effective radius
without overwriting the saved preference. A profile that denies snapping locks the player
control off; enabling it requires an edit to the mod profile.

Quick Build has its own inherited profile permission and personal on/off toggle, covering both
mexes and geothermal plants. Denying one resource feature does not automatically deny the other.
A profile that denies Quick Build locks its player toggle off; editing the profile can permit it.
Remember the personal toggle per mod, shared across versions, and preserve it while a profile
denies the feature.

Quick Build searches using the profile's default radius, capped by its maximum. It has no separate
personal radius slider initially. A personal manual-snapping radius of zero therefore leaves
Quick Build available when its own permission and toggle allow it. Holding Alt temporarily
bypasses both features. Profile permissions, defaults and limits share the reload rules below.

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

## Manual placement behavior

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

## Contextual Quick Build

### Activation and target priority

Activate only in normal contextual command mode, with at least one selected constructor able to
build the chosen resource building. An explicitly selected Move, Attack, Reclaim or other command,
including manual building placement, keeps its normal behavior. Quick Build follows the player's
configured contextual command button: classic and right-button order interfaces must both work.
It must not depend on temporarily arming manual placement and hoping that button accepts it.

Hovering empty ground over a qualifying metal patch or geothermal vent shows the exact selected
building's ghost at its legal placement. Ordinary unit, wreck and reclaimable-object targets keep
their contextual actions. The exception is an eligible completed resource building owned by the
player, which offers the upgrade described below. Unfinished buildings retain normal repair/assist
behavior. Allied and enemy buildings retain their normal contextual actions.

Use the profile search radius and the same exploration policy as manual snapping. Quick mex
construction requires a distinct deposit: uniform metal ground retains ordinary movement clicks,
including uniform regions above `SurfaceMetal`. Richer discrete deposits on a metal map remain
eligible. Owned-building upgrades remain available on metal maps, and geothermal vents are
independent of the uniform-metal rule. A distinct-deposit classifier suitable for TA still needs
source research and live verification; TADR's above-background footprint count alone cannot
establish this property.

Choose the highest extraction-rate mex available to the selected constructors that legally fits
the target patch. A lower-rate option can be chosen when stronger ones do not fit. Apply the same
rule to geothermal plants using energy output. Show the exact variant in the ghost. If no option
fits, retain normal contextual behavior. Definition metrics, build-list enumeration and stable
tie-breaking must be established against TA's data before implementation; do not substitute unit
names or tech-level guesses for the agreed output comparison.

### Orders and mixed selections

A normal Quick Build click replaces participating constructors' orders; Shift-click appends.
Constructors able to build the chosen variant receive its build order. Other selected constructors
that can assist receive ordinary Guard orders on the capable builders, distributed across them.
Combat units keep their existing orders. Assist orders persist after the resource building
finishes, continuing to help the guarded constructor until the player replaces the Guard order.
Appending more work must not create duplicate helper Guard orders.

Keep contextual mode active after a successful click, including Shift-click, so the next patch or
vent can supply its own ghost without selecting an icon. The preview and command must agree on
the building, placement, target and modifiers at the input boundary. A mouse move or a timed
preview update is not a prerequisite for a correct click.

A site already reserved by the player's queued resource-building order shows an **already queued**
indicator. Repeated Quick Build clicks on that site leave existing orders unchanged and consume
the command click: they must not fall through to a movement order or duplicate/cancel construction.
Normal order controls remain available to cancel those orders.

### Owned-building upgrades

Offer upgrades only for completed mexes or geothermal plants owned by the player. The selected
replacement must be buildable by the selection, legally fit after the old building is reclaimed,
and strictly improve extraction rate or geothermal energy output. Equal-output replacements and
weaker variants retain normal contextual behavior. The stronger variant shown in the ghost is
the one ordered.

An accepted upgrade issues reclaim followed by replacement construction. Normal click replaces
participating queues; Shift-click appends the sequence. Helpers use the same Guard behavior as
new construction. Do not reclaim unfinished structures for an automatic upgrade. Geothermal
power production stops while the plant is reclaimed and rebuilt. Verify the engine roots for the
whole sequence and prospective placement with the existing occupant; do not temporarily remove
occupancy or directly call simulation effect sinks to manufacture a green preview.

### BAR reference and deliberate differences

Checked BAR at commit `8146db69b41875b9673161c3f3bf181bbb18029a`:

- The [Quick Build widget](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/8146db69b41875b9673161c3f3bf181bbb18029a/luaui/Widgets/cmd_quick_build_extractor.lua)
  supplies mex/geo ghosts, upgrades and right-button construction. It suppresses build/Reclaim
  modes and classifies metal maps using the resource-spot list; its bare-ground branch suppresses
  both resource types on those maps.
- The [resource builder API](https://github.com/beyond-all-reason/Beyond-All-Reason/blob/8146db69b41875b9673161c3f3bf181bbb18029a/luaui/Widgets/api_resource_spot_builder.lua)
  compares extraction/energy values, checks the first resource option of each builder when choosing
  a variant, and gives secondary builders ordinary Guard orders. Upgrade eligibility includes
  allied buildings and tech/specialty rules. Its preview prepares a build command and refers to
  the separate Mex Upgrade Reclaimer gadget.

Impure's agreed rules require the configured button, priority for every explicit command, the
strongest legally fitting option, completed own buildings and strictly improved output. Uniform
metal ground does not disable geothermal Quick Build. BAR's spot-count heuristic is not a verified
TA metal-map classifier, and its widget does not establish TA's reclaim/build dispatch contract.

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

The [engine mouse-button map](../exe-reverse-engineering.md#the-in-game-mouse-buttons-what-a-click-actually-does-mapped-by-us)
already distinguishes contextual mode `1`, manual placement mode `14`, the interface preference,
left/right dispatch and cancellation. Use those mapped contracts as research starting points.
Before Quick Build implementation, trace build-list membership, builder/assist/reclaim capability,
selected-unit filtering, owned queued sites, completion and output fields, reclaim/build/Guard
roots, upgrade occupancy validation and button-down/up ownership. Stock build dispatch visiting
selected builders alone does not prove each can build the chosen variant. Publish Quick Build
ghosts through the existing game-thread preview path without moving engine calls to the renderer.

Before implementation, establish the input/update and click hook contracts by disassembly,
including footprint parity, map boundaries, exploration lookup and the engine's buildability
entry point. Read map/definition objects under the engine's lifetime and validated bounds. A
pointer probe or a short interval between checks and reads is not a safety argument.

## Verification before landing

Run the feature in the game; compilation and source inspection do not verify placement. The
required behavior cases are:

- Stock TA and each supported mod, with fresh defaults and edited general/version profiles;
  independent profile locks, Quick Build default/toggle, personal manual radius zero with Quick
  Build still enabled, inherited limits and saved preferences across versions.
- Normal deposits, occupied deposits with red previews, map edges and differing footprints;
  zoomed views, stationary-mouse Alt changes, immediate clicks and manually queued build orders.
- Uniform background-metal maps, above-background uniform regions and mixed deposit maps;
  coverage ties and values demonstrating that the rule does not maximize income.
- Unexplored terrain, discovered terrain under grey fog and fully revealed maps; scoring across
  exploration boundaries without reading unseen resources into the result.
- Geothermal sites with valid and blocked placements, including supported mod yardmaps.
- Quick mex/geo ghosts and orders with each configured command button, normal and Shift clicks,
  immediate clicks, Alt changes without motion, explicit commands and manual placement active.
- Strongest legal variants, stronger variants that do not fit, no legal variant, mixed constructor
  and combat selections, assist eligibility, persistent Guards and duplicate Guard prevention.
- Owned completed weaker mex/geo upgrades, own unfinished structures, equal/better buildings,
  allied/enemy buildings, unavailable reclamation, different replacement footprints and queued
  reclaim/build sequences; previewing an upgrade must not mutate occupancy.
- Queued-site indicators and repeated clicks preserving orders; ordinary targets above deposits;
  Quick movement on uniform metal ground, rich discrete deposits, metal-map upgrades and vents.
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
