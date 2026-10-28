# D. Resource snapping, Quick Build and mod profiles

**Implemented and replay-validated, 2026-10-06.** This page describes mex and
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

These are **Impure defaults**, including Stock TA. Stock TA does not ship TADR. Quick Build is
an additional Impure feature, separate from the inventory's native TADR snapping row.

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

### Profile files

General filenames are `retail.ini`, `mayhem.ini`, `prota.ini`, `twilight.ini`, `escalation.ini`,
`tazero.ini` and `unknown.ini`. An optional override is `<mod>@<version-bytes-as-lowercase-hex>.ini`:
Stock's `v3.1` is `profiles/retail@76332e31.ini`. The settings label shows the detected identity and
selected file. Encoding prevents an engine-supplied identifier from becoming a path component.

```ini
manual_snapping=1
quick_build=1
default_radius=3
max_radius=3
quick_build_default=1
```

Boolean values accept `0`/`1` or `off`/`on`; radii accept integers 0–8. Comments start with `#` or
`;`. Unknown keys, duplicate keys, embedded NULs, files over 4096 bytes and a resolved default
radius greater than the maximum are rejected. Overrides inherit missing keys. General files also
inherit omitted keys from the embedded defaults. The reader denies concurrent writers while
reading each file, then publishes the complete validated policy on the game thread.

Identity shares the replay browser's config/archive table and Twilight's menu label. The stock
policy additionally requires the stock config and archive formats. A custom package retaining
both stock identifiers cannot be distinguished from stock by this detector. The reported version
is the engine's printable version field (at most seven characters), or Twilight's label; a
loader's full package release is not necessarily present there. Exact overrides match that
reported identifier, not an assumed installer version.

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
`SurfaceMetal` background value. Prefer the highest count; ties favor the candidate-cell center
nearest the raw mouse position, using TADR's `(candidate + 0.5)` metric on both axes.
The actual building center depends on footprint parity, so this does not guarantee the physically
nearest building center on an even footprint axis. This scores qualifying coverage, not metal
income or the sum of metal values.
If no candidate qualifies, retain normal placement.

Manual and Quick Build mex selection share the complete-target check derived from the modern
TADR caller. When the chosen grid center differs from the raw mouse cell, search again around
that choice with radius equal to the larger footprint dimension, retaining the raw mouse for
distance ties. Accept only if the same footprint origin wins. The second search verifies the
choice; it does not extend the configured snap radius. A partial deposit clipped by that radius
therefore retains ordinary placement or contextual movement, instead of offering an offset mex.
The verification scores coverage without occupancy filtering, so an obstruction cannot make a
partial target count as complete. Quick Build additionally requires native site acceptance.

Occupied deposits remain snap targets for mexes. Their preview is red while ordinary engine
validation says construction is blocked. This supports preparing a reclaim-and-replace upgrade;
snapping does not grant permission to build over an occupied site.

Geo snapping uses the same candidate-cell-center metric among placements accepted by the engine's buildability rules for the
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
| A broad uniform region above the background level | Equal full-footprint scores favor the candidate-cell center nearest the mouse |
| Unequal richness values that are all above the threshold | Coverage determines the score; higher income is not guaranteed |

The uniform-region statement applies to candidates with the same qualifying coverage; map edges
and region boundaries can change their counts. It does not promise identical coordinates to
vanilla placement in every rounding case. Geo snapping remains independent of the metal threshold.
These TADR consequences are derived from source. Impure's generated-map regressions separately
exercise uniform metal equal to the background, uniform metal above it and rich discrete deposits
on metal ground.
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

Use the profile search radius, complete-target check and exploration policy of manual snapping. Quick mex
construction requires a distinct deposit: uniform metal ground retains ordinary movement clicks,
including uniform regions above `SurfaceMetal`. Richer discrete deposits on a metal map remain
eligible. Owned-building upgrades remain available on metal maps, and geothermal vents are
independent of the uniform-metal rule. The closed-component classifier below establishes the
distinct-deposit rule; TADR's above-background footprint count alone cannot establish it.

Choose the highest extraction-rate mex available to the selected constructors' build menus that
legally fits the target patch. A lower-rate option can be chosen when stronger ones do not fit.
Apply the same rule to geothermal plants using energy output. Show the exact variant in the ghost.
If no option fits, retain normal contextual behavior. The implementation reads definition output
metrics and each constructor's build menu by the page loader's rule (*Candidate source*, below),
with a stable definition-index tie-break. Unit names and tech levels do not determine output
priority.

### Candidate source

Quick Build offers what each selected constructor's **build menu** offers, the buttons the player
would press, and never reads the `CANBUILD` list (owner decision 23, 2026-10-28). `CANBUILD` is
the AI's list: the engine's own placement dispatch `0x419670` builds whatever a menu press armed
and reads no list, while the list carries AI-only types and every download entry whatever its
`MENU`. The menu **replaces** the list rather than being intersected with it: an intersection
would leave Twilight's ARMCK with nothing for a vent (its list has no geothermal) and nothing for
a deposit (its list's AAIMEXX is not on its menu).

- **Per constructor.** A builder type's menu is what it shows when it alone is selected: its pages
  `<NAME>0.GUI` (the orders page) and 1..min(count − 1, 7), the side's `DL` page standing in for a
  missing one, filled from the download entries, greyed by the engine's pass, and pressed as the
  engine's handler presses (a 16-character name, the pager and tab names refused, mobile types
  refused). The selection rule is unchanged: the union of the selected constructors' menus, the
  constructors whose menu holds the choice as primaries and the others Guarding.
- **The read.** `tagpu_buildmenu.c` is the rule, engine-free and unit-tested
  (`tagpu/ddraw/tests/buildmenutest.py`); `tagpu_resource.c` feeds it the files through the
  engine's own file calls on the game thread and keeps the type numbers per level generation and
  watched side. The engine map's *The left sidebar* has the rule's addresses and the
  *Resource placement and contextual order contracts* row its calls and bounds.
- **Replay and multiplayer.** Nothing changes for the recorder: the menu decides which ordinary
  order a click makes, the order is what is recorded, and playback never consults the menu.
  Each peer reads its own files for its own clicks, as it draws its own build buttons.
- **What it changes.** Stock: nothing, MEASURED offline over all 45 stock builders on both sides
  (no extractor or geothermal difference against `CANBUILD`). Twilight: ARMCK builds ARMAMEX on a
  deposit and ARMGEO on a vent, where it built the AI's AAIMEXX and had nothing for a vent; CORCK
  builds COREXP and CORGEO. A `CANBUILD`-only type, such as Twilight's AI extractors, is no longer
  offered.

| Verification of the candidate source | Result |
|---|---|
| The rule against the Python model of it, every builder and side of the six compatibility setups (`buildmenutest dump`) | 0 differences: stock 106, Twilight 280, TA Zero 267, Mayhem 224, ProTA 136, Escalation 380 builder-side cases |
| `buildmenutest.py`, native, ASan/UBSan | 34 checks pass |
| Twilight 2.0b98, Wine, MEASURED 2026-10-28: one ARMCK and one CORCK on the resource fixture map, Quick Build clicks | ARMCK: deposit → ARMAMEX, vent → ARMGEO, ground → Move; CORCK: vent → CORGEO, deposit → COREXP, whose nanoframe appears. One `resource: menu` line per builder type for the level (ARMCK 3 pages, 40 types, 2 extractors, 1 geothermal; CORCK 3 pages, 41 types; 0.7 ms each) |
| Retail, the same driver | ARMCK: ARMMEX, ARMGEO, Move; CORCK: CORGEO, CORMEX, as `CANBUILD` gave. ARMCK 4 pages (the fourth the side's `DL`), 19 types; CORCK 4 pages, 21 types |
| `resource-refusals`' candidate-source step | run by the landing's replay gate (`tareplay.py landing`) |

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

A site reserved by the player's queued resource-building order shows the ordinary ghost without
caption text. Shift plus the configured contextual command button removes a selected constructor's
matching plan; a queued upgrade removes its adjacent reclaim/build pair. Other selected
constructors' matching plans are cancelled too, one per constructor. Unselected constructors'
queues, unrelated orders and persistent helper Guards remain intact. The queued definition may
differ from the strongest variant currently offered by the selection.

A plain repeat click leaves the queue unchanged. A Shift-click with no selected owner also leaves
it unchanged. Both consume the command click rather than issuing Move or duplicate construction.
Cancellation does not undo construction or reclamation that has already happened; an unfinished
resource building retains native contextual repair/assist priority.

**Whether a plan is queued at a site is decided before whether its ground is free.** A unit
standing on a queued plan's footprint or walking across it, or a wreck on it, makes the engine's
site test `0x47D2E0` refuse the site; that refusal never hides the plan, so the repeat and the
Shift-cancel still reach it instead of a native order being issued in its place. This holds for
a new site's search (`site`, for mex and geo plans) and for an owned building's upgrade
(`quick_choice`): occupied ground or another plan's yard is the click's only as a queued plan's
site, and a free site with no plan in the way is decided as it was. The ghost on such a site takes
the altitude the engine's build cursor gives a blocked site (`0x47D820`). The unit under the
pointer is still the click's target first: a click on a unit standing on the plan is a click on
that unit, as it is for the stock game.

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

## Implementation and replay ownership

`tagpu_resource.c` chooses sites on the game/input thread. `tagpu_profiles.c` owns file parsing,
inheritance and effective settings. `tagpu_replays.c` supplies the shared mod identity; the menu
refreshes the profile label and two separate controls. Polling runs on the engine flip’s game
thread at 500 ms menu intervals, independently of packet publication or a render consumer, and
stops for the entire match. Returning to the main menu permits it again.

The raw point is captured after the existing zoom/viewport mouse repair. The observer on
`0x4197D0` updates only `main+0x2CAA` before the engine computes the manual rectangle and flags.
The gates on `0x498F70` and `0x499100` handle their respective configured contextual button; all
other paths call through. A manual left click recomputes placement before normal dispatch.

Quick Build publishes a cursor/type/rectangle override into the frame packet, leaving the engine
in contextual mode. The renderer never runs site tests or order roots. It displays the chosen
variant's ordinary ghost without caption text. Preview tests save/restore the two
engine build-site scratch words `0x51E684` and `0x51E688`.

A new build goes through `0x43ADC0`, the engine's ordinary append/replace order root. Reclaim and Guard eligibility
come from `0x43F0E0`; build action names come from `0x438760`. Upgrades issue reclaim followed by
an appended build. Helper constructors retain persistent Guards; the engine may prepend its own
assist/reclaim work. Selected combat units receive no order from this path. Queued builds are
identified by the engine descriptor's placed-build capability, excluding factory build counts.
Actual yard occupancy bits determine queued collisions; a site the occupancy test refuses is
still matched against the queued plans before it is dropped (above). Plain repeated clicks consume the input
without changing orders. Shift cancellation selects the actual queued node and removes it through
the exact native root `0x439F80(unit,node)`. The mouse toggle root `0x43AFC0` ignores build type and
treats positions within 16 world units as a match; it cannot distinguish adjacent one-cell vents.
New Quick plans use `0x43ADC0` so a Shift append never toggles a neighboring plan.
Geo reservations compare a unique geothermal feature anchor touched by the G yard, normalizing
bounded footprint aliases; footprints spanning multiple distinct vents have no cancellation identity.
Upgrade pairs require an adjacent resolved reclaim and an unstarted non-head build with no handler,
StopBuilding flag (`+0x42` bit `0x400000`) or owned object. Its destructor only unlinks its reference, so the preceding reclaim
remains live across build removal and can then be removed exactly. No destroyed node is reused,
and cancellation never appends an order. Only selected constructors are cancelled; persistent
helper Guards remain ordinary orders.

The upgrade test projects the old building's post-reclaim yard-bit clear into a bounded private
copy of the candidate definition/yardmap. The engine's occupancy test ignores exactly the old
unit ID. No preview mutates the grid or live definitions. Contested old footprints are refused,
because their destructor can restamp another unit and cannot establish that projection. Details
and call sites are in the engine map's *Resource placement and contextual order contracts*.

The capture wrapper is independent of zoom activation and uses pristine conversion when zoom is
disabled. Resource hooks require that producer to be installed. Visuals Undo restores raw
per-mod preferences, so an effective profile cap never becomes the saved radius through Undo.

All engine reads share the input thread's level lifetime. Unit indices, the controlled player's
inclusive unit partition, definition indices, map products, exploration bits and descriptor
indices are bounded before addressing memory. Yard reads use the loader's allocation contract.
The build-menu read runs on the game thread where the engine reads its own build pages, and
bounds what it takes: the page probe's and GUI_Load's paths to their buffers, a file to 1 MiB
read exactly, the download records below their count and five entries a record, the watched
side below the sides loaded, and every type number below the def count. Its table holds type
numbers only, one table a level generation.

Resource and profile scratch is classified as presentation: simulation never reads it, no address
survives into the simulation, and the module refuses puppet seats. The orders it creates are
normal recorded engine state. Playback, in-place restoration and fresh-process seeks reproduce
those orders and their reclaim/build/assist effects through the existing recorder and engine
roots. The approved viewer/input metadata fields are listed below; recorded simulation slots
and tolerances are unchanged. The owner also approved omitting the recording player's native
immediate-command UI click (`0x41A0F1`, return `0x41A0F6`), including Stop, as the console click
already is; the commands and their simulation effects remain checked.

### Quick Build's metal-ground classifier

Manual snapping retains the footprint-coverage rule. Quick mex building additionally requires a
closed, fully explored eight-connected component above `SurfaceMetal`, with a discovered
background neighbor. The search arena is 129×129 cells around its seed. A component reaching that
arena's edge, touching both opposite map edges on either axis, or meeting unexplored neighbors
is conservatively refused. This keeps uniform above-background metal maps as normal movement,
including small maps with a vent interrupting their metal surface. Discrete deposits on metal
ground remain eligible. Very large deposits and deposits not yet fully explored may therefore
need manual placement. Owned upgrades and geo sites do not need this component classification.

### Automated regressions

`tools/resource_fixture.py` generates original small terrain and policies inside isolated private
game folders. Its optional stronger geo and capability-refusal fixtures derive private test units from installed game
content; none of those archives is tracked. `tools/resource_regression.py` drives native input and
requires actual order/preview results in the live lane before replay comparison. A fixture fails
if its input merely leaves an old queue unchanged.
The mex fixtures verify the live mouse-interface value. `resource-classic` selects left-button
orders through native Interface options after launch; a prelaunch registry setting is insufficient
because the launcher replaces it with right-button orders.

**The cases run in lockstep.** A case holds the world through the DLL's test pacing (`pause at
<T>` and `resume` in `tagpu_replay.trigger`, a test launch's own; `tagpu_replay.c`, *THE PAUSE AT
T*) and advances it only by exact ticks, so every click and key lands on a held world and the
world a click meets is a function of the ticks the case asked for, not of wall-clock pacing. An
input's direct effect -- orders, selection, the placement rectangle, the cursor mode, a
`resource:` line -- is read once after `tacli keys` returns, which is after the engine has
consumed every event (`input-firewall.md`, *The transport*). Simulation progress -- a Guard's
first step (90 ticks at most), Stop settling (30), a unit reaching a site (900, in steps of 15)
-- is read on the held world and again after each advance. The camera and the menus, which no
step drives, keep their wall-clock waits. The world is handed back running when the case ends,
so the lane's own holds follow; `viewer`, run inside the null twin's pause, holds nothing. A
comparison of a queue before and after a click reads both on the same held tick.

| Fixture | Input behavior exercised | Replay checks |
|---|---|---|
| `resource-mex`, `resource-classic`, `stock-resource-mex` | Manual mex/Alt, manual/Quick center agreement, partial-target refusal at the snap boundary, independent toggle, both mouse layouts, plain repeat no-op, selected-only Shift cancellation across variant changes, preceding Guard preserved, requeue, and Shift cancellation with a unit (ARMPW) standing on the queued site, its index read in the site's grid cells first | L/P/R/Q |
| `resource-upgrade`, `resource-queued-upgrade` | Own mex→Moho reclaim/build, plain replacement or Shift after a Guard, mixed helpers, persistent Guard dedup, combat unaffected; queued-pair cancellation/requeue preserves the preceding and helper Guards | L/P/R/Q |
| `resource-core-upgrade` | Native save/load preflight with all ten slots active; installed Comet Catcher, native CORACK→CORMOHO contextual click, reclaim/build pair, stance, progress and completed Moho at tick 7800 | Native loader coverage + L/P/R/Q; last comparison after completion |
| `resource-plain`, `resource-uniform`, `resource-mixed` | Background/above-background uniform metal and rich patches; manual geo/Alt, Quick geo cancellation/requeue preserving a preceding Guard | L/P/R/Q |
| `resource-geo-upgrade` | Stronger private geo variant on metal ground, queued reclaim/build, plain repeat no-op, pair cancellation/requeue, persistent helper Guard | L/P/R/Q |
| `resource-nearby` | Two adjacent one-cell vents remain distinct; Shift appends both, cancels only the clicked plan, preserves a preceding persistent Guard and requeues | L/P/R/Q |
| `resource-locked`, `resource-inherit` | General locks, exact-version inheritance/clamping, effective controls and policy frozen despite edits during the match | L/P/R/Q |
| `resource-priority` | Explicit Move, Alt, equal own mex and personal Quick toggle off | L/P/R/Q |
| `resource-nozoom` (stock limits) | Manual/Alt and Quick Build when zoom and widening are disabled | L/P/R/Q |
| `resource-refusals` | Reclaim-ineligible constructor, Guard-ineligible helper retaining its queue, eligible primary and helper fallback; the candidate source: RSNGCK, whose menu is ARMCK's and whose `CANBUILD` holds only RSNMOHO (a download entry with no `MENU`), Quick-Builds ARMMEX on the deposit and never RSNMOHO | Native catalogue preflight + L/P/R/Q |
| `resource-frontend` | Untouched/touched Visuals Undo preserves a raw radius above the current cap; raising the cap recovers it with packets disabled | Frontend preflight + L/P/R/Q mex |

A queue is read in one consistent piece: each round of `Check.chain` reads the unit's head and
every node found so far in one peek, which the DLL evaluates in one game-thread call, and the
chain is taken only from a round that links the head to its end. A preceding order that a
comparison keeps is a Guard of an owned unit, read after its first step: unlike a Move it never
completes on its own, and its compared fields are fixed from that step on
([the engine map](../exe-reverse-engineering.md#the-ground-guards-node-and-what-the-order-dispatch-does-with-a-handlers-result-disassembled-2026-10-09)).
`resource-refusals` keeps a Move for its Guard-ineligible constructor; the world stays held from
that order to the comparison, since the click issues every order at once.

L records real input and simulation; P plays it on puppet seats; R restores in place at three
sampled ticks, compares before/after state and continues; Q seeks from a fresh process. The
queue-cancellation helper advances one tick before the cancellation and one after it, so both
the queued and absent states reach recorder capture points before requeue.
Native acknowledgements or later render frames alone are insufficient. The
landing harness also checks event attribution, effect counts, address values, puppet writes and
the complete main/.data images. Paused viewer samples exercise camera movement, GUI-consumed
mouse messages, all valid coordinate bits and native selected-type changes. Additional paused
camera/selection views and the final view each retain exact comparison of carried simulation state against the
first paused view. Eight owner-approved viewer/input metadata fields remain local, totaling
30 bytes: the active screen pointer (4), mouse-event clock (4), bitmap-font pointer (4), keyboard
enqueue/dequeue indices (4 each), mouse button/modifier flags (4), saved GUI gadget status (4)
and displayed unit type (2). The 60 recorded global slots / 78 dwords and comparison tolerances
are unchanged. [Recorder completeness](demo-recorder-completeness.md) records their owners,
native evidence and the native Stop-button sound exception.

Fixtures name their resource/profile/UI source dependencies in `fixtures.json`; `tareplay landing`
selects them when those sources change, in addition to its SIM_FEATURE rules. Changes to the
replay harness or native driver select every fixture. This landing's final passing note remains a gate, not an
assumption from the existence of these fixture definitions.

### Verification limits

`tools/resource_upgrade_stress.py` adds a completion check on the real Comet Catcher map.
Its `prepare` command reads the installed map and generates one mex plus one advanced
kbot for every metal-feature anchor, varying approach bearings and distances. `--phase`
rotates those assignments; `--sides arm|core|mixed` selects the constructors. A private
supply unit removes resource starvation; the constructors, mexes, Mohos and terrain
retain their installed definitions. Generated archives and scenarios remain local.
`run` applies that scenario in an owned instance, issues real configured contextual
mouse clicks, and separately records refusals, accepted reclaim/build pairs, old-mex
removal, Moho creation and completion. It runs in the same lockstep as the regression cases:
the clicks land on a held world, and each snapshot of the native orders reads one, 150 ticks
after the last (`resource-core-upgrade`'s `check-core`: every 30 ticks to T+600).
An accepted upgrade that removes its mex and loses its build without a replacement
is a failure; a surviving unfinished build at the observation bound is reported as
incomplete. This is a native stress test, not a replay comparison. The existing mex
upgrade fixtures assert the queued pair and helper behavior; they do not require a
completed stock Moho at every Comet Catcher deposit.

The stress reproduction exposed a native **CORE COB slot exhaustion**, rather than
an overlap in the measured failed case. CORACK's old RequestState/Stop/close chain
overlaps its new StartBuilding and StopMoving callbacks; all eight records are busy,
and the new RequestState START needs a ninth. Its refusal prevents INBUILDSTANCE,
so the native accepted nanoframe decays. Explicit native construction from idle at
the same bot/site succeeds. The engine map records the script chain, native node
states, trace and non-overlapping footprints; an earlier unrecorded play incident
cannot be assigned that exact cause from this reproduction alone.

The owner selected **ten script records**. Allocation, all native loop bounds,
guards, saves and trace agree on that capacity. Older native and guarded eight-slot
saves retain their own validation bound and import with the two extra slots zeroed.
The stock-limits control stays at native eight slots. This fixes the reproduced
ninth-slot handoff; arbitrary mod callback demand is not proved bounded by ten.

| Verification on the ten-slot runtime | Result |
|---|---|
| CORE, all 81 Comet deposits, phase 0; 8 bearings and 6 requested distances | 61 accepted, all 61 completed by 7611 elapsed ticks; 20 initial placement refusals; no abandoned or unfinished accepted build |
| ARM, all 81 deposits, phase 1; generated starts inside the border | 67 accepted, all 67 completed by 8303 elapsed ticks; 14 initial refusals; no abandoned or unfinished accepted build |
| Native eight-slot save → guarded ten-slot load | All eight active records preserved on human and local-AI probes |
| Guarded ten-slot save/load | All ten active records preserved on both probes; automated preflight also reaches native `cob_save` and `cob_load` on the coverage build |
| Trace preflights with recording disabled | All ten guarded and eight native active records preserved on human/AI probes; both allocator observers and the guarded stack observer reached |
| Refusal and stock resource replays with trace isolated | All 75 checks pass, including 29 exact paused comparisons, six exact restores and both strict image gates; allocator sink remains counted |
| Full-pool START / CALL | 16 / 3 assertions pass; refusal retains the documented scheduling behavior |
| Portable COB regression | 41 tests pass, including high-slot links, old-format bounds and transactional load refusal |
| Long completed-Moho replay before the keyboard-index exclusion | Ten-slot run: 36 checks pass, including completion, effects, all 14 paused comparisons and three exact restores; only keyboard indices `0x51F48E`/`0x51F492` fail the strict image gate |

The diagnostic ARM phase-1 sweep completed 66 of 67 accepted upgrades; its one
remaining bot was generated near the map's lower border and never moved or reclaimed
its mex. It did not match the reported post-reclaim failure. Generated starts now
remain at least 128 world units from the border and record their actual approach;
the ten-slot ARM rerun completes all 67 accepted upgrades, including that case. This
corrects the generated test start rather than changing the native pathfinder or upgrade orders.

The owner approved two four-byte keyboard-ring index leave rows on 2026-10-05,
`0x51F48E` and `0x51F492`, keeping those local input indices with the viewer.
Builds, upgrades, unit/player state and effects retain their existing comparisons.
The earlier combined ten-slot run after that approval completed 36 live fixtures, with 1431 passing
checks, 504 exact paused comparisons and 35 passing strict image gates. The refusal fixture's
image gate still identifies the minimap camera rectangle and displayed-unit type; the weapon
fixture could not start because its worker retained an older unit catalogue. A sequential
stock-resource-mex → wpn-badtgt run with content-boundary catalogue invalidation passes all
84 checks, including both image gates. Trace preflights preserve all ten guarded and eight
native active slots on human and AI probes; guarded coverage reaches the allocator and stack
observer. Trace is removed before recording, because its allocator hook displaces the ordinary
sink counter. The latest combined run completed all 37 live recordings, but disk exhaustion
interrupted the remaining replay lanes. Its completed report also identifies three strict
UI-image failures: CORCK's displayed type in `resource-nozoom`, the native gadget-status latch
in `resource-plain`, and Shift in the latest mouse flags in `resource-upgrade`. Their native
writers and the unsuccessful gadget-latch diagnostic are recorded in the engine map. No new
exclusion was applied in that run. Redundant binary artifacts from completed passing fixture reports
were removed while preserving logs, coverage, summaries and failing/incomplete cases. A fresh
whole-landing rerun completed 1448 passing checks, including 519 exact paused comparisons and
33 strict image gates. Four image checks still name the approved displayed-type/gadget-status
fields. Its stock-limits rewind lane could not create the battle armies because a prior
two-seat playback persisted `NumSkirmishPlayers = 2` and scenario loading did not replace it.
The corrected launcher supplies the declared slot extent before startup; starting from count
2 it creates four commanders and all 1428 battle units without errors. Offline driver and
harness tests pass. The owner-approved ten UI bytes are now applied, with the recorded globals
unchanged. The subsequent whole-landing run finishes all 37 live, playback, restoration and
fresh-seek fixtures: 1479 checks pass, including 519 exact paused comparisons and 36 strict
image gates. All 17 resource image gates pass, including completed Moho and native stock mex.
The corrected stock rewind setup and its continuity/AI checks pass. The sole failure is the
unchanged `stock-secondary-orders` control's native cosmetic pools: 514 unlisted image bytes
differ. At its T451 checkpoint, live and ordinary playback both hold 15 native explosions;
fresh seek holds zero. A focused fresh-worker rerun at the same checkpoints reproduces 469
failing bytes in those same pools, with every other check passing. Seek deliberately clears
effects; the raised `SIM_ENGINE` pools are declared but also excluded from recording until
their layouts are implemented. This is an existing recorder gap, not evidence of a mex order
or construction mismatch. The owner accepted a bounded known gap on 2026-10-06: only this
native stock control's unlisted-image check may accept differing bits in the three exact
cosmetic-pool ranges, totalling 41204 address bytes. Every other check remains intact, and
all 17 resource image gates stay strict. See [recorder completeness](demo-recorder-completeness.html)
for the bounds, option and outstanding snapshot-layout work. The saved whole-run and focused
comparisons pass with that option and still fail without it. The new full landing rerun
passes all 37 fixtures, 1480 checks, 519 exact paused comparisons and all 37 image gates;
all 17 resource image gates stay strict. Its native control samples T301 and accepts zero
cosmetic bytes, also passing the default strict comparator at that checkpoint. The earlier
T451 gap remains reproduced. Coverage reaches all 11 changed function/region requirements,
and the passing `replay-roundtrip:` note is on `cfe3771f`; `landing --check` passes.
Two independent HIGH landing reviews found no surviving code defect. Their three LOW
documentation findings are corrected: this tie metric, the order-root write table and the
COB page's whole-gate status.

The null twin now samples camera positions across the map's native scroll extents and selects
ARMACK and RSNRACK in separate refusal-fixture views. The prior small camera sweep never set the minimap rectangle's
right-coordinate bit 6, and selecting RSNRACK (type 280) never set displayed-type bit 2;
ARMACK is type 4. These are native paused input samples, not new exclusions. See the engine
map for the indirect rectangle writer and measured values.

The ten-slot DLL's Wine compatibility run has 18 expected setup outcomes, one documented
gammata inactive gap and zero unexpected outcomes across 19 setups; all 15 two-peer matches
pass. Windows was not run for this change. Compatibility does not validate every mod's mex
or geothermal menu.

The synthetic fixtures cover the behavior listed above; live inspection additionally verifies
the resource settings panel, queued resource ghost, frontend hot reload/error recovery and
normal Comet Catcher terrain/features. The generated `RS` maps
are intentionally flat green. The compatibility suite checks the supported packages' startup,
battles and networking; it does not exercise every mod's resource footprint and build menu.

Not established by these fixtures: every supported mod's geothermal yardmap, very large or
contested upgrades, variant fallback under every obstruction, all exploration-boundary/fog
combinations, every edge/zoom/footprint combination, all ordinary allied/enemy/wreck priorities,
or automated frontend profile-error/recovery and cross-version preference persistence. These
remain targeted regression opportunities; source bounds and refusal paths do not substitute for
claiming those cases were run.

The released [v2026.6.8 scorer](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/tahook.cpp#L175),
[selector](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/tahook.cpp#L292) and
[caller](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/tahook.cpp#L650) establish the native
coverage/tie/stability and geo rules. The inventory separately checks the native mod bundles and
their activation gates; this implementation does not imply that native E/Z enabled snapping.

## Future work

During multiplayer, compare players' mod-level feature on/off policies for parity. Once in-game,
report a mismatch in chat with the player's name and issue, using the form
`WARN PlayerName: issue`. This is a TODO, not part of the first snapping implementation. Peer
capability discovery, transport, comparison fields and aggregation of several issues still need
a design.

Downloading mods and activating them without restarting TA is also future work. Investigate the
engine's archive, definition and script lifetimes before defining an activation mechanism. The
agreed reload boundary is the menu, with requests deferred throughout a match.
