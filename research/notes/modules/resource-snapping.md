# Resource snapping, Quick Build and profiles

Resource snapping, contextual Quick Build and profiles.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.108 Resource snapping, contextual Quick Build and profiles (`tagpu_resource.c`, `tagpu_profiles.c`) — 2026-10-04

Manual mex/geo placement and a separate contextual Quick Build option use the existing input,
packet and ghost paths. The [resource page](../tadr-port/resource-snapping.html) gives policy,
behavior, fixture coverage and gaps; the engine map's *Resource placement and contextual order
contracts* gives the allocation/lifetime proof and disassembly.

| Seam | Mechanism |
|---|---|
| `0x498DA0`, redirected call `0x499221` | Always-installed wrapper captures resource coordinates after zoom repair, or after pristine conversion with zoom disabled. Resource arming requires this producer |
| `0x4197D0` | Nine-byte entry observer chooses manual placement, original code computes corners/flags and tests the site |
| `0x498F70`, `0x499100` | Six-/seven-byte gates for left/right handlers, each `ret 4`. Only the configured contextual button is consumed; paired button bookkeeping outside these handlers remains native |
| `0x47D2E0`, `0x47C780`, `0x47D820` | Engine site/altitude queries on the input thread. Quick queries preserve scratch words; an upgrade uses a private yard projection of an uncontested old footprint's clear. A queued plan is matched before the site test's refusal drops a site, and its ghost then takes the blocked-site altitude `0x47D820`, which writes nothing |
| `0x4290F0`, `0x4BBC40`, `0x4BB5B0`, `0x4BBD00`, `0x4BB710`, `0x4BB7C0`, `0x4BB5D0`, `0x488B10` | Quick Build's candidates: each selected constructor's build menu, read on the game thread by the page loader `0x41ACE0`'s rule on the first use of a builder type in a level (and again when the watched side changes). The probe and the DL fallback choose the file, the TDF loader's file calls read it into our buffer, `tagpu_buildmenu.c` applies the download fill, the greyed pass and the press's tab test and 16-character cut. Type numbers only are kept, per def table (its base and count) and level generation |
| `0x43F0E0`, `0x438760`, `0x43ADC0`, `0x439F80` | Native eligibility (zero action byte refuses), action lookup, append/replace and exact removal. Upgrade is reclaim→appended build; non-building constructor helpers Guard primaries via `0x43AFC0`, combat units untouched |
| `tagpu_packet_pub.c` → native ghost and marker | Chosen type/green rectangle and queued/selected-cancellation bits copied into the packet. Engine stays in contextual mode. Queued resource ghosts have no caption text. No new renderer-owned engine access or GL object |
| `0x4C63A0` flip observer / `0x46A3E0` game return; shared replay-menu identity | Config/archive names and Twilight's label select a general profile plus an optional exact-version override. Game-thread menu polling remains live without packets or a render consumer; native game flips freeze policy and in-game options cannot unfreeze it |

Defaults permit and initially enable both features for Stock/M/P/T, and lock both off for
E/Z/unknown. A player can set manual radius zero with Quick Build still on. Alt bypasses both.
Manual and Quick mex selection share a footprint-sized stability check with raw-mouse distance
ties. A partial deposit at the configured search boundary cannot supply a Quick ghost or build;
the native contextual action remains available. The check adds no state or engine writes.
Quick Build offers what each selected constructor's build menu offers (owner decision 23), not
its CANBUILD list, which is the AI's: MEASURED offline, the two give the same extractor and
geothermal candidates for all 45 stock builders, and on Twilight ARMCK now builds ARMAMEX and
ARMGEO and CORCK COREXP and CORGEO. The menu read writes no engine game state, so the *fields
we write* table gains nothing from it. The engine's file layer it calls does allocate on the
engine heap: each open makes a handle through `0x4BB2E0` → `0x4D83B0` (`0x4BB2F5`), and the
close `0x4BB5D0` frees it (`0x4D85A0`, `0x4BB63D`), exactly as for the engine's own reads. It reads
the defs, the download records, the side table and the GUI files, every index bounded at use. Unit/definition/grid/
exploration/descriptor values are bounded, and owned unit walks use the engine's validated
inclusive player partition. Engine heap order nodes have the input thread's sole-mutator
lifetime; nothing retains a pointer across a simulation iteration or level teardown.

Shift plus the configured command button cancels one matching queued resource plan per selected
constructor through `0x439F80`; a queued upgrade cancels its adjacent reclaim/build pair. A gated
unstarted, non-head build's callback-free destructor preserves the preceding reclaim's lifetime.
Unique canonical vent anchors distinguish adjacent geo sites; new plans use `0x43ADC0` to avoid
the mouse toggle root's 16-world-unit ambiguity. Plain repeats, unselected owners' plans,
unrelated orders and persistent helper Guards remain unchanged. The cancellation adds no patch,
simulation-owned state or replay exception.

Resource scratch and profile state are presentation inputs; simulation never reads those tables.
Puppet seats are refused. Every effect is an ordinary recorded engine order and its normal
simulation roots. Resource fixtures assert native live behavior and compare playback, three
in-place restores and fresh seeks. Cancellation tests fence simulation ticks around removal,
so requeue cannot hide the absent order image. The replay landing selector also follows their declared
source dependencies; changing the harness or native driver selects the whole suite. The owner approved three
four-byte viewer metadata rows: the active GUI stack pointer (`main+0x531`), mouse-event clock
(`main+0x561`) and current bitmap-font pointer (`0x51F524`). Two keyboard-ring indices add eight
bytes; mouse flags (`main+0x55D`, 4), saved gadget status (`main+0x11E7`, 4), and displayed type
(`main+0x37E9E`, 2) add exactly ten more approved bytes. These eight viewer/input fields total
30 bytes, with the 60 recorded globals slots / 78 dwords unchanged. The owner also approved an expected-absent
row for the recording player's native immediate-command UI click (return `0x41A0F6`), including
Stop; its simulation orders remain checked. No tolerance changes. Paused viewer samples verify
that camera movement, GUI mouse input and native selection leave all carried simulation state
unchanged. See [recorder completeness](../tadr-port/demo-recorder-completeness.html) for the field
owners, disassembly and exact comparison protocol.

Scenario loading writes its explicit roster's seat extent through the existing locked
prelaunch registry store. A prior two-seat playback therefore cannot truncate a subsequent
four-seat fixture. Native stock-limits validation creates four commanders and all 1428 battle
units from a count-2 starting store; the subsequent full run also passes its stock rewind
lane, including exact return/fresh-seek frames, timeline continuity and AI orders.

**Open:** multiplayer policy parity warnings and mod download/activation. The fixture table names
what was exercised; it does not establish every mod yardmap, obstruction, fog boundary or
edge/zoom combination. The 129×129 closed-component classifier conservatively refuses large or
partly unexplored deposits, and contested upgrades are refused. Profile identifiers describe
what the engine reports, which may be shorter than an installed package version.

Resource regression preflights also check Visuals Undo against raw per-mod preferences and
refresh the engine catalogue for privately derived capability-refusal constructors. The
no-zoom fixture exercises pristine conversion with both zoom and widening disabled.
