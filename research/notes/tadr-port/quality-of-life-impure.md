# D. Quality of life — evidence: what Impure has

A read-only pass made 2026-10-04 over Impure's own source at `faabb488`, with the rows later
landings changed re-read at `2b40c748` on 2026-11-08, asking which of TADR's
player conveniences Impure has and which it lacks. It compares functions, not module names: each
row puts the whole feature-module and patch inventory beside a direct reading of the menu, HUD,
marker and input code that overlaps it. It is one of three passes behind
[the coverage table](quality-of-life.md). [The bundled-TADR pass](quality-of-life-bundles.md) and
[the newer-builds pass](quality-of-life-newer.md) settle what the mods ship and switch on; this one
leaves that alone.

TADR is read in `vendor/TADR` at `dcff5ddeb6bd1030e3f452c0f16e5f005850f62f`, and every upstream link
below is pinned there. Its changelog opens with an undated `????.??.??` section
([tdraw.txt](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L63)),
so **presence in that source is not proof of a release or a bundle**. Impure's code is cited by
file and function. No game was run: this is a source audit, not a re-test of the stock behaviours
Impure keeps.

Four rows have changed since the pass, and say so where they stand: mex/geo placement snapping
([resource snapping](resource-snapping.md)) and replay camera follow
([the replay's players](demo-recorder-player-cards.md)) are covered, line/rectangle/ring placement
with spacing is partial ([the placement module](../modules/placement.md)), and the megamap is
covered differently, by Impure's own mega map ([the plan](../mega-map-plan.html)).

## Which TADR player conveniences are still missing?

When the pass was made, [the port's groups](overview.md#the-groups) listed group D as not planned,
which overstated the gap. Impure already had build-cursor and queued-build ghosts, crisp stock
health, group, order and range markers, zoom, a renderer menu and a scalable HUD. The rows below are
the player functions Impure actually lacked, with partial features called out.

| Missing player feature | Impure's code, and the precise scope | TADR source |
|---|---|---|
| Mex/geo placement snapping | **Closed since the pass**: `tagpu_resource.c` snaps manual mex/geo placement and adds contextual Quick Build, replay-validated 2026-10-06 ([resource snapping](resource-snapping.md)). At the pass there was no cursor-nearest metal or geo choice: the queued-site patch (`build_site_test` and `qs_taken` in `tagpu_patches.c`) calls stock `0x47D2E0` and only turns its yes into a no for an occupied queued site; it never picks another position. | MexSnap is documented apart from WreckSnap; the mod's executable must opt in, and it is off by default. [tdraw.txt:342](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L342) |
| Reclaim/wreck snapping | No input or command path snaps a reclaim click to a target. The order markers (`tagpu_order.c`) draw targets and existing queues; they choose nothing. The reclaim sweep (`tagpu_sweep.c`, [the module](../modules/reclaim-sweep.md)) is an area order — a dragged ring orders the selected reclaimers onto its wrecks, trees or both — not cursor snapping. | WreckSnap snaps reclaim commands to features. [tdraw.txt:344](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L344) |
| Drag queued build/move orders to edit their positions | Impure snapshots and draws the engine's own order list (`tagpu_order.c`). There is no marker hit-test and no drag path, and keys pass through `fake_WndProc` (`wndproc.c`). The one order-node write is the engine-equivalent last-seen-target cache. | [Move Queued Orders, tdraw.txt:362](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L362); `tahook.cpp:2057` implements `DragUnitOrders`. |
| TA Hook line/row/ring building, spacing and optimized DT rows — **partial since the pass** | `tagpu_place.c`, with the shapes in `tagpu_placegeo.c`, lays lines, rectangles and rings from a Shift-drag with a building armed, spaced by `placespacing=` (Alt+Z / Alt+X) ([the placement module](../modules/placement.md); TADR beside BAR in [the coverage table](quality-of-life.md#queue-editing-and-line-building-tadr-beside-bar)). Optimized DT rows and Build Split are not built. | `tahook.cpp:1082` `WriteDTLine`, `:1097` `CalculateLine`, `:1208` `OptimizeDTRows`, `:1525` `CalculateRing`, `:1655` `ClickBuilding`. [TA Hook](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tahook.cpp#L1082) |
| Construction patrol set to reclaim only through Hold Position | No patrol-action filter in Impure's patches or feature modules. The [merge exploration](../tadr-merge-exploration.md#d-ui-quality-of-life) lists it under D. | [tdraw.txt:368](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L368) |
| Builders stay put, or scatter, while guarding factories | No command-tick patch for it. The menu's rows (`s_row` and `s_vrow` in `tagpu_menu.c`) are rendering and display settings, not factory-guard behaviour. | The mod must enable the executable byte at file offset `0x101F1F`; the player then chooses stay-put, Cavedog or scatter. [tdraw.txt:170](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L170) |
| Build under own mobile units, moving blockers aside | The queued-site patch (`build_site_test` in `tagpu_patches.c`) can only turn stock's yes into a no. It cannot allow a placement stock refuses, or move a blocker. | [tdraw.txt:166](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L166) |
| Live game-time clock and wind/tidal readouts on the resource bar | The HUD code (`tagpu_hud.c`) maps and scales the stock panel and top bar only. The replay HUD's clock belongs to playback and, in a live game, to an open rewind's strip only (`tagpu_hudstate_mode` in `tagpu_hudstate.c`); there is no live game-time readout. | [tdraw.txt:228](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L228). The TA Zero build hides wind and tidal on purpose and keeps the clock: [config.h:43](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/config.h#L43). |
| Map-owned DTs and fort walls drawn regardless of LOS | No owner-11 exception: the native draw (`tagpu_native_frame` in `tagpu_native.c`) rejects unexplored anchors and fogged owners other than the watched one. None of TADR's visibility-patch addresses appears in Impure's source. This concerns map-owned unit obstacles, not ordinary map-feature sprites, which Impure renders. | `TABugFix.cpp:195–213`: map-feature units take owner 11, and `0x469A7B` bypasses to `0x469AAF`. [TADR patch](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/TABugFix.cpp#L195). **Upstream comments out the three owner-assignment writes** (`:2409–2411`), so what a supported mod's own exe and content carry decides it. |
| Shared whiteboard lines and labelled map markers | `tagpu_mark` draws world UI markers — health, group digits, build footprints, the selection rectangle, stock orders and ranges (`tagpu_mark.h`, `tagpu_order.c`) — not a shared whiteboard. There is no annotation model, no marker editing and no recorder transport. | tdraw owns AlliesWhiteboard; the recorder sends Rec2Rec `0x00` ([the recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface)). [whiteboard.cpp](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/whiteboard.cpp) |
| Allies' camera rectangles and camera sharing in a live game | The camera code (`tagpu_zoom.c`) transforms the local eye and the minimap box. It neither sends camera movement nor draws a peer's viewport. | `.sharemappos` sends positions as recorder `0xFC`; `maprect.cpp` draws them ([the recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface)). [maprect.cpp](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/maprect.cpp) |
| Allies' live resource/income panel | The replay HUD's stats window shows each recorded player's resources only in a replay: `tagpu_hudstate_mode` (`tagpu_hudstate.c`) gives the replay mode only under `tagpu_replay_live()`, and the live HUD's record carries no seats and no stats. There is no live allied economy panel. | `cinomce.cpp`'s panel relies on recorder data from `0x28` and `0x16` ([the recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface)). [cinomce.cpp](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/cinomce.cpp) |
| Accessible chat backdrop and crisp chat font | Impure redraws the engine's GUI text and scales the whole HUD. There is no chat-specific backdrop or font option among the menu rows (`tagpu_menu.c`) or the settings (`tagpu_settings.c`). | Released 2026.7.3: [tdraw.txt:110](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L110). Which bundles carry it is a separate question. |
| Unicode text and input | The input shield (`deliver_key` in `tagpu_shield.c`) makes `WM_CHAR` from the low byte, with no Unicode composition, and the GUI text pass (`tagpu_text.h`) draws byte glyphs. | tdraw creates UnicodeSupport and routes messages and drawing to it: [iddrawsurface.cpp:210](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/iddrawsurface.cpp#L210), [iddrawsurface.cpp:1292](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/iddrawsurface.cpp#L1292). |
| Enhanced selection hotkeys: idle constructors and factories, category-aware combat selection, same-type double click | No selection-hotkey module, no consumer of the `CTRL_B`/`CTRL_F`/`CTRL_W` categories and no idle-unit selector. The key cases in `fake_WndProc` (`wndproc.c`) pass through; Impure's own key paths are injection and shielding. This is **TADR's enhanced behaviour**, not a claim that stock Ctrl+B/F/S fail. | `ExternQuickKey.cpp:195–241`: Ctrl+S, Ctrl+B idle construction, Ctrl+F idle factory, same-type select at `:195`; categories at `:831–891`. [handler](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/ExternQuickKey.cpp#L195), [category parsing](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/ExternQuickKey.cpp#L831) |
| TA Hook's configurable F11 resource-sharing macro | No F11 macro and no setting for one. The menu rows are rendering and display only, and keys are forwarded. | `tahook.cpp:417` intercepts F11; `:920–928` runs the macro's lines through ChatText. [macro](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tahook.cpp#L417) |
| Building rotation (`Rotations=` and the facing UI) | The ghost is drawn at the fixed heading `{0, 0x8000, 0}` (`ghost_one` in `tagpu_native.c`). The unit-key reader (`fbi_at_read`, installed by `tagpu_datakeys_units_install` in `tagpu_datakeys.c`) reads `PreviewPieces=`, veterancy and transported explosions, not `Rotations=`. | [tdraw.txt:758](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L758) |
| Extra build-preview keys: alternate model, per-facing pieces, facing the enemy commander — **partial** | The basic ghost and `PreviewPieces=` exist. `PreviewObject3D`, `PreviewPiecesS/E/N/W` and `PreviewFaceOpponent` have no reader or consumer: the ghost draws the type's own model at the default heading (`ghost_one`; `fbi_at_read`). | [tdraw.txt:139](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L139); [per-facing pieces and facing the opponent](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L857) |
| Reload-progress bars for `reloadbar=1` weapons | The bar loop (`tagpu_mark_gather` in `tagpu_mark.c`) draws health and the group digit only, and the packet's unit row (`tagpu_packet.h`) has no reload fields. [C's evidence](data-keys-evidence.md#part-2-alerts-and-display-nomapweaponalert-and-reloadbar) routes `reloadbar` to D. | [tdraw.txt:545](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L545), in the undated section: not evidence of a release. |
| Stockpile `stocked +queued` and loaded-transport `loaded/capacity` counters | The same bar loop draws health and the control group. The packet carries cargo links for drawing carried units (`tagpu_packet.h`), but there is no counter text. Only the new counters and a configurable group-digit colour and outline are missing; the digit itself is drawn. | [tdraw.txt:297](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L297), in the undated section. |
| Team-coloured build/reclaim nanolathe streams and nanoframes | Team colour reaches unit models and ghosts, not this effect: particles use the engine's stored colour (`tagpu_sfx.c`), and the nanoframe wire is the stock animated blue (`tagpu_posedraw.c`). | [tdraw.txt:808](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L808), in the undated section. |
| Allies' *unstarted* build queues, broadcast and drawn | The player's own queued sites and ghosts exist. The order snapshot (`tagpu_order.c`) walks the watched player's block and broadcasts nothing that exists only in an ally's UI. | The source has the feature, but `ALLIED_BUILD_QUEUE_ENABLE` defaults off and is **disabled in every build at `dcff5dd`**, so the coverage table carries it among the newer-build rows: [config.h:110–124](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/config.h#L110); [tdraw.txt:82](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L82). |
| Configurable chat position, channel split and retained scrollback | HUD scaling maps the stock chat with the rest of the GUI (`tagpu_hud_to_engine` in `tagpu_hud.c`). There is no chat anchor, channel or history setting and no custom chat ring. | [ChatPosition.h:3–28](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/ChatPosition.h#L3); [ChatLayout.h:7–17](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/ChatLayout.h#L7). |
| Local `.mute`/`.unmute` for one player's chat, pings and drawings | No chat filter and no command path for it. TADR's own initialization is suppressed (`DllMain` in `dllmain.c`; [the takeover](../compat/takeover.md)), so nothing carries over from the installed mod. | [PlayerMute.h:3–32](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/PlayerMute.h#L3), a per-config option. |
| Percentage auto-share thresholds (`+setshareenergy 50%`) | No percentage-threshold state, recompute or command patch, and the menu adds no share preference. | [SharePercent.h:7–23](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/SharePercent.h#L7): an Escalation-only option in this source. |

**How the rows were decided.** An absence combines the whole feature-module and patch inventory
with a direct reading of the overlapping menu, HUD, marker and input functions; it does not rest on
a search for TADR's class names. TADR's initialization is made inert before Impure's patches are
set up (`DllMain`), so a missing feature cannot survive quietly through the bundled DLL.

**How the coverage table uses them.** Release and bundle presence is a column of its own there,
and a feature counts as mod-enabled only where shipped config or content enables it. A feature
that is an ordinary player toggle is not counted "mod disabled" because a package omits the
preference ([TADR's feature configs](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/config.h#L43)).

**What this pass does not show.**

- It reads no mod binary or content. Current upstream config is not evidence of what a mod
  ships; [the bundled-TADR pass](quality-of-life-bundles.md) settles that.
- No game was run, so the stock behaviours Impure keeps are not re-verified here.
- Whether map DTs show through fog depends on the owner bytes baked into the mod's exe as well as
  its DLL. The native fog gate shows only that Impure lacks the bypass.

## Which apparent gaps are already covered, or only partial?

Player-visible gaps are kept apart from differences in how the renderer does things. TADR's
megamap is not ported as its separate icon-map screen; Impure's mega map covers it differently
([the port's groups](overview.md#the-groups)).

- **Build ghosts, queued ghosts, `PreviewPieces=`: present.** `ghost_record` (`tagpu_native.c`)
  gathers the cursor ghost and the queue ghosts into Impure's unit renderer; `fbi_at_read`
  (`tagpu_datakeys.c`) parses `PreviewPieces`, and `ghost_compute` adds the hides of the COB
  Create script to the ghost's mask. "Nanoframe ghost preview" is therefore not a missing row,
  only the narrower partial row above.
- **Health bars, control-group digits, selection boxes, build footprints and stock
  order/waypoint/range markers: present**, drawn at zoom resolution (`tagpu_mark.h`,
  `tagpu_order.c`). These are not TADR's extra stockpile or reload bars, nor its whiteboard.
- **Mouse-wheel world zoom: present**, with cursor anchoring, a camera tween and zoom-aware input
  (`tagpu_zoom.c`). TADR's separate tab/wheel megamap is not ported as a separate view; **since the
  pass** Impure's mega map is the world zoomed out to the whole map with units drawn as strategic
  icons ([the plan](../mega-map-plan.html)), so the coverage table marks the row covered
  differently.
- **HUD polish, high resolution, FPS, settings:** Impure has its own renderer and visual settings
  and UI scale (`tagpu_menu.c`, `tagpu_hud.c`). A generic "HUD polish" row would name no real
  functional gap, so there is none.
- **Team colour on unit models: present** (the packet's `local_logo` and per-owner baked team
  texture frames, `ghost_one`). It is separate from TADR's team-coloured build and reclaim
  particles.
- **Replay transport, seeking, bookmarks, a player's LOS and each player's resources:
  present** in Impure's own recorder and the replay HUD (`tagpu_hudinput.c`,
  [The replay HUD in game](../hud-in-game.md)).
  Demo recording and replay are not missing.
- **Replay camera follow: present since the pass.** At the pass, Jump, Follow and Free cam were
  drawn disabled and the hover said "Camera was not recorded", so nothing matched TADR's
  `.lockon`. Since then Jump and Follow — now on the replay HUD's player rows, which replaced the
  cards — play a recording's camera stream for any seat with a recorded camera sample
  (`tagpu_replay_follow_request`, called from `tagpu_hudinput.c`), in skirmish and multiplayer
  recordings, and a seat without one still says "Camera was not recorded"
  ([the replay's players](demo-recorder-player-cards.md);
  TADR's side: [the recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface)).

The coverage table keeps itself manageable by grouping: category-select conveniences are one row,
the extra build-preview keys one partial row, stockpile and transport counters one row, chat
anchoring and scrollback one row. Mex and wreck snapping stay separate, because Escalation removes
only mex snapping ([the Escalation delta](../tadr-merge-exploration.md#the-escalation-delta)).

None of this claims Impure's rendering of a feature is pixel-identical to TADR's; it records
whether the player function exists.

## Which adjacent multiplayer conveniences are also missing?

These belong to group E, not D ([the port's groups](overview.md#the-groups)), so the coverage
table leaves them out. A broader reading of "quality of life" would put them in a second, short
table, still without simulation limits, fixes or security mechanisms.

- **Team-aware start positions, `+autoteam` / `+randomteam` and working battleroom team
  controls:** E, not planned. [tdraw.txt:321–341](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L321)
- **Several local AIs in one multiplayer game, one human plus AI from the lobby:** E, not planned;
  the recorder's `MultiAILimit` bypasses the one-AI check
  ([the recorder's limits](../tadr-merge-exploration.md#a-limits),
  [tdraw.txt:232](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L232)).
- **Host game-speed limits `.syncon lo hi` / `.syncoff`, autopause, `.ready`/`.voteready`
  unpause, `.votego`/`.forcego`:** E game control
  ([game control](../tadr-merge-exploration.md#e-game-control-unit-transfer-anti-cheat)). Impure's
  replay pause and speed (`tagpu_hudinput.c`) govern a viewing process, not these live network
  controls.
- **Commander warp start, and taking a disconnected player's units with `.take` / `.takecmd` /
  `.give` / `.stopgive`:** E, not planned
  ([game control](../tadr-merge-exploration.md#e-game-control-unit-transfer-anti-cheat)).
- **Vote to reject:** E, not planned
  ([group E](../tadr-merge-exploration.md#e-multiplayer-anti-abuse),
  [tdraw.txt:144](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L144)).
- **Sight and radar shared among allies:** not a display convenience. Sight feeds simulation
  acquisition and cloak and submerged behaviour, so it is the owner's decision under E
  ([LOS shared among allies](../tadr-merge-exploration.md#line-of-sight-shared-among-allies-a-decision-for-the-owner)).
- **The `.noshake` button:** stock already has `+noshake`; tdraw adds a button, not an engine
  function, so it belongs with the live game-control buttons
  ([tdraw.txt:233](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/tdraw.txt#L233)).

The lag-switch guard, share-abuse guard, challenge-response, CRC integrity checks, limits and
simulation fixes stay out of a QoL table altogether: they are E/security or A/B scope.

These E items rest on the port's status, the merge exploration's inventory and this pass's
whole-module audit, not on a fresh disassembly of every live-network entry. Which of them the
shipped recorder DLL actually contains is a bundle question, and the recorder's git HEAD is known
not to equal the shipped plugins
([what ships is not what the source holds](../tadr-merge-exploration.md#what-ships-is-not-what-the-source-holds)).
