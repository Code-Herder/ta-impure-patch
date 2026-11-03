# The in-game wiki

The in-game wiki's sidebar and pages, and its stage drawn in 3D by the world passes.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.12c The in-game wiki (`tagpu_wiki.c`, `tagpu_wikistate.c`, `tagpu_wikilay.c`, `tagpu_wikistage.c`, `tagpu_vk_hud.c`; every Vulkan level)

The wiki button in the top bar's right-hand row and, when it is pressed, the kit's window over the
game (90 % of the screen, centred): the title "Wiki" and its close button, the tree, the selected
entry's demos, and the entry's column -- title, subtitle, badge, the stage with its demo playing,
the caption and keys strips, the controls and the text -- drawn from `art/wiki/kit/` at the
device's resolution on the Vulkan lane in every level, a replay's included. While the window is
open the engine takes no input, and a single-player level is paused. Try it yourself hands the
stage to the player: the demo's scene simulated on the game thread, driven by the pointer, the
buttons and the keys the window takes anyway. An entry with `"frame": "game"` (Watching, under
Replays) is laid out as the theatre instead -- the stage across the window over a row of numbered
demo tabs, with About and the Entries drawer over it -- and its stage is a whole game frame: the
running mod's two bars in its own art, the watcher's column, the map, and a replay HUD of the
stage's own whose controls the demos press on a replay of the wiki's simulation, or the player in
Try it yourself. A live game entry (Rewind a skirmish) shows the player's own screen instead -- seat
0's column, selection and fog, the top bar's numbers -- and the live HUD, the rewind button and its
strip, on a live skirmish the wiki's simulation plays. A takeover entry (Take over a replay) is a
replay with the replay HUD's Take over set -- the picker, the Forks bay and its lanes -- whose
takeover turns it into that live game as the seat taken, the top bar in that seat's side's art. A
players entry (Players and the camera) is a replay with the replay HUD's players column -- Jump,
Follow, Sight, the overlay switches and the stats window -- on a game screen smaller than its map,
seen through a camera. Beside the stage's map is the running mod's left
sidebar, the only build menu ([the in-game wiki](../ingame-wiki.html), *The left sidebar on the
stage*). Design and status:
[the in-game wiki plan](../ingame-wiki-plan.html), landings 2 to 5; the engine facts: [engine map](../exe-reverse-engineering.html), *The in-game wiki's engine
questions*. **No byte patch and no detour**: the input is taken in hooks Impure already owns, the
pause through `tagpu_menu.c`. GDI draws neither the button nor the window, so the wiki cannot open
there. The top bar's Impure Config sprocket is drawn by this layer too, beside the wiki button, while
the wiki is drawn: drawn only, its press stays `tagpu_menu.c`'s ([settings](settings.html) §2.12,
*Who draws it*). The wiki button and the sprocket are emblems with no plate (`wiki_emblem`,
`config_emblem`, each with its `_down`), like the live HUD's rewind button.

| site | what we do there | thread |
|---|---|---|
| `fill_frame` (`tagpu_packet_pub.c`), after the effects' models | `tagpu_wiki_fill`, given by value the controlled player's side (PlayerInfo `+0x95` of the record `main+0x1B63 + 0x14B·byte[main+0x2A42]`, the record index held below eleven, -1 when it or the pointer does not validate; `wiki_side`) and `main+0x37EFA == 1` (`wiki_iface`), read there: a pair other than the last restarts the open window's demo. One `TAGPU_PK_WIKI` record (open, the pause taken, the pointer's client point, the held control, in Try it on a live frame whether that point lies on the frame's world (`try_world`, `stage_world` through the hit map last read: the one answer the view's ghost and the drawn cursor both follow), the sprocket the same frame's draw left to this layer (`tagpu_menu_bar_cog`, set inside `DrawGameScreen` before this fill), the page's selection, folds, scrolls and playback, and the text's context -- the Interface Type and the layout side whose casting names the units, which the render thread fills the page's text with) into the packet's last area `wiki_off`/`wiki_len`; while open, the demo's `WIKISIM` stepped by the clock (at most 8 ticks) -- or, for an entry with `"frame": "game"`, its replay (`tagpu_wikireplay.c`, the demo's game played as the replay HUD plays a recording: [the in-game wiki](../ingame-wiki.html), *The game frame on the stage*) stepped in whole steps while their summed cost stays within `WIKI_REPLAY_FILL` (64; at least one step a fill, the rest owed; a step's cost is never fewer than the game ticks it runs -- a step whose viewer's trip acts on its clock tick is charged the most a step runs, 64 -- so a fill runs at most 64 game ticks), the view of the world shown -- the replay's game, or a live replay's past simulation while the past is shown, as the human seat sees it (seat 0, or the seat a takeover took: `tagpu_wikisim_view_as`) -- held while a seek runs and otherwise copied and filtered to the frame's view by mode (`tagpu_wikiframe_view`: the watcher's for a replay and a takeover entry's watching, the player's while a live rewind is idle or open, the past's under the live simulation's column, which `tagpu_wikistage_side` copies in; the watcher's and the past's with a replay's overlays -- the seats' cursors, clicks and selections, every seat's to the watcher and the human seat's in the past -- and a players entry's with the eye and viewport from the replay's camera and, under Sight, the viewed seat's fog, units and minimap pass), its frame record filled after the fill's last step (`tagpu_wikiframe_record`, values only, into the record's `frame`, 4 600 bytes, a live frame's rewind state, top bar numbers and the human seat's cursor among them, a takeover entry's Take over block -- the picker, the lanes, the forks listed, the human seat -- and a players entry's players block -- Sight and Follow, the switches, the folds, the stats window and every seat's samples of its measure, `tagpu_wikireplay_stat` -- and `top_side`, the human seat's or the Sight seat's layout side, `tagpu_wikiframe_seat_side`) and its sound drained past the view's `quiet` -- and the view copied by value into the record's `stage` (`tagpu_wikistage_copy`; the nano spray's live particles with it, `WIKISTAGEPART part[WIKISTAGE_PARTS]`, x, y, z and colour, 16 bytes each, at most the simulation's pool of 1024, `WIKISTAGE_TRUNC_SPRAY` set when the simulation dropped one), then each unit's posed model into the view's pose pool (`tagpu_wikistage_pose`: each primitive's MOVE, TURN and flags out of `tagpu_wikisim_model`, 4096 `WIKISTAGEPIECE`s of 20 bytes over all the units, a unit's run `pose_at`/`pose_n`; a unit past the pool or `WIKISTAGE_PIECES` draws at rest and sets `WIKISTAGE_TRUNC_POSE`), the record then `sizeof(TAGPU_PK_WIKI)` = 172 744 bytes on the i686 build against a 328-byte head (the theatre's `about`, `drawer`, Leave's `leave_held` and the replay's serial `rep_serial`, incremented whenever the replay is made or freed, in the head; the reader takes `stage` and `frame` only from an area of the whole size, `tagpu_pk_wiki_stage`, `tagpu_pk_wiki_frame`); none, and the window closed, when the HUD's pass is off (GDI, or the pass refused) or the wiki's layout refused (`tagpu_wikistate_refused`). A whole record that does not fit sets `TAGPU_PK_TRUNC_WIKI` (`0x1000000`) and is placed as its head (`tagpu_wiki_head_only`: no stage, a message on it), the shortfall still the whole record's so the slot's growth aims at the stage; a head that does not fit either carries no record, and the first fill of the next publish closes the window (`tagpu_wiki_carried` records what the published fill carried; a fill redone for the same publish, `s_fillAgain`, does not count). The trigger `tagpu_wiki.check` (every demo played to its end for every side of the layout and both Interface Types, each `tagpu_wikisim_hash` logged as `<entry>/<demo>/<side>/<iface> <hash>`; a game entry's demo played through `tagpu_wikireplay` with the viewer's script, its `tagpu_wikireplay_hash` logged the same way, then one cost line per replay: the game ticks its steps ran, the milliseconds, the start and the dearest step) is answered from the fill, `tagpu_wiki.dump` (writes `tagpu_wiki.json`, with the bytes carried, a live replay's state under `replay.live` (`live_json`: the rewind, the arm, the timelines with their seats, the human seat, the past simulation and the world shown as the human seat sees it), a takeover replay's under `replay.tk` (`tk_json`), the frame record's Take over block, the drawn lanes `hud_lanes`, and the clock's own account since the demo started: `clock_s`, the seconds it played, each fill's clamped at 0.25 s, and `clipped`, the ticks the 8-tick bound took off) from `tagpu_wiki_carried` after the record is placed. The test harness's pacing, read on the same beat while open: `tagpu_wiki.pause` (`ENTRY DEMO TICK`: that demo picked afresh and let play, to pause on exactly that tick, before the step past it) and `tagpu_wiki.turbo` (K, while the file is there: a playing demo steps K ticks a fill in place of the clock's, K at most 8, so a fill costs no more than the clock lets it) | game |
| `frame_valid` (`tagpu_packet.c`) | refuses a wiki area whose length is neither `TAGPU_PK_WIKI_HEAD` (no stage) nor `sizeof(TAGPU_PK_WIKI)`, or that lies outside the slot | render |
| `tagpu_overlay_draw`, after `tagpu_hudstate_present` | `tagpu_wikistate_present`: the record laid out by `tagpu_wikilay_build` in the wiki unit (screen height / 720), its titles, subtitle, paragraphs, demo titles and tree leaves filled as they are read (`tagpu_wikitext_fill`) for the record's Interface Type and layout side, whose casting `casting_of` takes from the published WIKISIDE through `tagpu_wikiside_get` -- immutable and never freed, the side bounded against `WIKI_SIDE_SIDES` and the layout's sides, anything else filling with each entry's own names -- the button in the bar's own pixels from `tagpu_hud_live`, in the rewind button's slot unless the same frame's live HUD draws that button, and the sprocket in the engine's slot for it when the record says so (`config`, 1 up or 2 down, any other value none); the stage's view copied out of the packet, every count bounded and every string ended (`view_bound`, the spray's `npart` to `WIKISTAGE_PARTS` among them), and built by `tagpu_wikistage_build` inside the page's bezel, each op tagged with its range (`WIKISTAGE_IN_*`); for a frame:game entry the record's `frame` copied and bounded as data (`tagpu_wikiframe_bound`), the view drawn as the whole game frame (the column, both bars as `HOP_BAR`, the emblems, the viewport clip, the TOP range with Leave's question and the viewer's cursor), and **the stage HUD** laid out twice by `tagpu_hudlay_build` on this file's own `HUDSTATE` and `HUDOPS` (statics, never the replay HUD's, whose hits never enter `tagpu_hudstate_drawn`: the stage cannot drive the real replay) -- the boxes, then the hint at the viewer's point, the simulation's own cursor on a live frame (its shape and age from the record; in Try it at the player's pointer, only with the record's `try_world`) or the player's pointer -- from `tagpu_wikiframe_hudstate` at 100 % of a screen the frame's size, the live HUD for a live record, and for a takeover's watching the replay HUD's Take over set as `tagpu_hudstate.c` sets the game's (the forks, the picker, the lanes, the button down while the picker is asked, sought or open), and for a players record the Players set and the stats window, its groups by `tagpu_hudlay_stat_groups` over the record's samples and its chart drawn in **the stage HUD's own chart slot** (slot 1, `HUD_TEX_CHART_WIKI`; the replay HUD's is slot 0); a return glide's point, and a trip to or from Back to main recording, resolved among the controls as drawn at its press (`tagpu_wikiframe_pressed`, on a second `HUDSTATE` of this file's); the lanes that HUD drew (`tagpu_wikiframe_lanes`) kept for the command record; **the bars** composed by `tagpu_wikibar_compose` from the published side art and the record's numbers (zero for a replay, the memo's on a live frame), the top bar in the art of the record's `top_side` (the taken seat's side in a takeover's live game, the viewed seat's under a players replay's Sight, else the load's) and the bottom bar and PANELSIDE's column in the load's, only when the side, the top bar's side, the colour, the frame's size, that art or a number changes (128 KB of indices and 512 KB of RGBA, statics), each new composition bumping the generation `tagpu_vk_hud.c` sends whole as `HUD_TEX_BAR`; the spray's particles are data there too, each drawn as a 2 × 2 game-pixel rect only for a colour inside `0xA1..0xA7` and a point on the map; the stage's cursor decoded once from the published arena's sequences through the presented palette (`tagpu_pal_live`) into a 32-cell sprite sheet (`HUD_SPRITE_CELLS`: the arrow's, move's and reclaim's 20 frames), again when `tagpu_pal_serial` moves, and sized by the sharp layer's `cursorscale=` (`tagpu_gui_cursor_scale`, written by `tagpu_gui_present`'s poll on the same thread); `tagpu_vk_hud.c` sends the sheet whole as `HUD_TEX_SPRITE` when its generation moves. **The sidebar**: the view names its layout by a value (`WIKISTAGE_LAYOUT_NONE`, `_FIXTURE`, `_MOD`), resolved here to a layout this thread holds itself (`side_resolve`: the fixture of the wiki's published data, or the running mod's through `tagpu_wikiside_get`; a `_MOD` that does not answer `WIKISIDE_READY` resolves to none, and the stage draws the column and its minimap alone; a value out of range draws no sidebar, `view_bound`). With the mod's layout, `side_bind` composes the column every frame (`tagpu_wikiside_compose`, 128 × 1024 RGBA8, with the PANELSIDE of the side the view names, bounded by the composer against the layout's sides) and bumps its generation exactly when the indices, the coverage or the rows differ from the last column's, compared byte for byte (two buffers, the new column composed into the one not drawn; 5.9 µs a frame for 480 rows and 12.9 µs for 1024 when nothing changed, i686 under Wine, MEASURED 2026-10-27); `tagpu_vk_hud.c` sends it whole as `HUD_TEX_SIDE` (sampled nearest) when the generation moves, and hides the `HOP_SIDE` instances of a frame whose generation the device does not hold yet (`hide_tex`), so a column is never drawn from another frame's texels. The texture is made with the pass (one more descriptor set in the pool) and freed in `tagpu_vk_hud_down` | render |
| `tagpu_overlay_draw`, after `tagpu_wikistate_present` | `tagpu_wikiworld_frame`: the stage's ground in 3D for this frame (§2.12d) -- the terrain, feature and target hand-overs (a feature the view lists reclaimed handed over without its quads, `tagpu_wikiscene_dead`) from the view `tagpu_wikistate_scene` answers and the arena `tagpu_wiki3d_get` published, each stamped with the frame | render |
| `vk_present` (`tagpu_vk.c`) | before the HUD's prepare, the wiki's stage in 3D is prepared and the frame's picture of the stage decided once (§2.12d, `tagpu_wikistate_image`); `tagpu_vk_hud_prepare` then builds the wiki's lists in paint order (`WIKILISTS`) -- the page's base layer, the stage's ops under its image, those over it (every op of the stage under on a whitebox frame), a frame:game stage's HUD (its base layer, then its top layer, in the replay HUD's art), the frame's TOP range, and last the page's top layer, About and the drawer -- through `tagpu_huddraw_build_list` as a third layer of the HUD's pass (a list's `HOP_CHART` reading its `chart_slot`'s image, each slot sent and generation-gated on its own; one instance range of 32 768 quads a slot shared by one partition, `tagpu_huddraw_partition`, main batches first; one glyph atlas; the kit's parts uploaded after the HUD's); after the HUD's top layer and the UI drawn again inside it, `tagpu_vk_hud_record_wiki` draws the page and the ops under the image, the wiki's world target is composited into the map's rect, scissored to the frame's viewport (`TAGPU_WORLDTGT.clip`, intersected with the block's own in `tagpu_vk_world_record`), `tagpu_vk_hud_record_wiki_over` draws the ops over it, and `tagpu_vk_gui_record_cursor` draws this frame's cursor quad once more over it all when the window is closed, or open with the pointer in the picture and not over the stage of a frame in Try it yourself (`tagpu_wikistate_cursor`: the record's `try_it`, its pointer and the frame's stage canvas, all of one frame), where the stage draws the simulation's own cursor | render |
| `tagpu_zoom_frame_end` | `tagpu_wikistate_cmd`: the drawn frame's hit map (up to 292 controls: the page's 160, a stage HUD's 128 and Leave's question's 4; client pixels, topmost first -- the page's top layer, the question, the stage HUD cut to the stage's canvas, the page -- each with its source, `WIKIHIT_PAGE`, `_HUD` or `_DIALOG`, since a key such as `play` is both the page's and the HUD's), the stage HUD's playhead run as drawn and the game tick its end stood for (and on a live strip the span it stood for, `hud_first` and `hud_G`), on a takeover's watching frame with the lanes open the fork each lane showed in its `lane:<k>` order (`nlane`, `lane_id`), on a game frame the image's rect and clip (`img`, `clip`, read by the state dump alone), the entry it drew, each panel's scroll as drawn and its limit, and each scrollbar's track and thumb, into the command record's `TAGPU_CMD_WIKI`, emptied as it is copied | render |
| the command record's take (`tagpu_packet_pub.c`) | `tagpu_wiki_take`: the map copied, every number bounded (`nlane` to three, each `lane_id` to 1..`RECR_TIMELINES` − 1, −1 past `nlane`), for the game thread's hit tests, scroll limits and lane presses; `TAGPU_CMD_WIKI` is 10 072 bytes on the i686 build | game |
| `tagpu_shield_wndproc`, after the `WM_TAGPU_*` cases | `tagpu_wiki_message`: open, every input message on the shield's swallow list plus `WM_SYSKEYDOWN`/`UP` -- a left press acting on the drawn frame's control or scrollbar (a press on `demo:`, `play`, `replay` or `try` only when that frame drew the entry selected now), the wheel scrolling the panel under the pointer and a track press paging it, each from the scroll asked held to the drawn limit, a move dragging a held thumb (a real move with the left button up ends a real press's drag; a frame that drew no wiki moves no scroll); in Try it yourself, a move over the drawn frame's `stage` control fitted to the world (`tagpu_wikistage_fit` on that control's rect and the frame's unit) as the simulation's move, a left or right press there as a move and a down, a release anywhere as the up of a held button, Shift, Alt and Ctrl, Alt+Z / Alt+X and E (a down on every key repeat, as the game's keyboard reaches RECLAIM) as downs and ups, all into the game thread's `WIKISIM` (a press on the stage's sidebar is such a press: the pointer is clamped to the content rect, the column with the map, and the simulation's sidebar rules act on it); closed, any press on a control of a drawn frame that still showed the window open (the frame or two after a close, a double-click on X) taken and owed its release, a left press on the button, and the paired release, which goes on when it re-locks the mouse (unlocked, closed). On a frame:game entry the page's `about` and `drawer` open one and close the other, a pick closes both, and in Try it yourself a press on the stage HUD or on its Leave's question (by the hit's source) goes through the press machine (`tagpu_wikiframe_press_down`/`_drag`/`_up`, the game HUD's rules) to `tagpu_wikireplay_press`, the track's tick read off the run the render thread last drew and rounded on the span it stood for (a replay's 0 to `hud_end`, a live strip's `hud_first` to `hud_G`; `track_tick`), and only while that frame was a game frame of this entry drawn from a packet of this replay (`frame_current`, the command record's `rep_serial`; a release on any other map lets the control go); a lane's control takes the fork its lane showed in that frame (`drawn_lanes`: an id counts only while it names a live fork of the running replay, and the release acts only when the lanes then drawn name the same fork at that place, so a list a frame old costs a press that does nothing). A Watching frame's stage has no world point (`frame` in the command record), so no move or press there reaches a simulation's cursor; a live frame's world takes Try it's moves and presses as the human seat's (seat 0, or the seat a takeover took; a takeover entry's watching frame takes none) (`live_world`: the topmost hit under the point the page's `stage`, the point fitted through it and the frame's unit and on the column or the map), a move held and sent once before each replay step and each press (`live_flush`), a press's own point replacing it, and every move, key and button reaching the replay through `tagpu_wikireplay_input`, which takes them only after a fork; an up goes to a live Try-it replay whatever frame is drawn ([the in-game wiki](../ingame-wiki.html), *Across the threads*). The press opens nothing while a rewind or Leave's question holds the pause (`tagpu_replay_rewind_state`, `tagpu_hudinput_leave_pending`). The frozen point is the press's `lParam` through `mouse_client_to_game`. A release of a button the engine holds (`TAGPU_WIKI_FROZEN`) goes to the engine at the frozen point through `deliver_engine`, unless the shield is armed; `tagpu_wiki_cancel` on `WM_KILLFOCUS`, `WM_CAPTURECHANGED` and a deactivating `WM_ACTIVATEAPP`, which also gives Try it's simulation the up of everything it holds | game |
| `deliver_mouse`, `deliver_key`, `WM_TAGPU_CHAR` (`tagpu_shield.c`) | `tagpu_wiki_mouse` (before the store to `g_ddraw.cursor`; `TAGPU_WIKI_FROZEN` delivers the release at the frozen point), `tagpu_wiki_key` (before `set_key`), `tagpu_wiki_char`: the injected path, the same rules | game |
| the two doors into TA's window procedure: `fake_WndProc`'s tail (`wndproc.c`) and `to_game` (`tagpu_shield.c`), past the display-only ring's drop | `tagpu_wiki_engine_got`: the left, right and middle buttons and the keys the engine is handed down and not yet up, whose releases the wiki never keeps | game |
| `WM_MOUSELEAVE` (`wndproc.c`) | while the wiki holds input, the leave forgets the client point and goes no further, so the engine is not told the pointer left | game |
| `fake_GetCursorPos`, `fake_GetKeyState`, `fake_GetAsyncKeyState` (`winapi_hooks.c`) | while the capture word is set: the frozen opening point (no store) and 0 for every key. `fake_GetCursorPos` stores its live point through `tagpu_wiki_cursor_store`, under the critical section the open and the close hold (`tagpu_wiki_init` makes it in `DllMain`'s attach) | any, the engine's mouse thread included |
| the art loader's thread (`tagpu_hudart.c`, `wiki_load`) | `tagpu_wikilay_check`: the kit and the data refused, and the wiki not published, when the kit lacks an icon or the `toast` style the window and the stage draw, or the page drops an op, run, text, hit or group (`HUDOPS.overflow`) in any of its checked states -- closed with its button held, open with its close button held, every entry's every demo playing at 1× and ended at 2×, with Try it's button held -- at 1024 × 768, 1280 × 720 or 3840 × 1080, every scroll at 0, the text filled at Interface Type 1 with each entry's own names. The build itself refuses only a missing argument or its memory, so the overflow is what the states test; other scrolls, captions, held keys, screens and the castings' names are not built. The parts and the roles' styles are refused by `tagpu_wikilay_load` itself | the loader |
| teardown wrap's entry (`tagpu_reclaim.c`) | `tagpu_wiki_level_end` after `tagpu_hudinput_level_end`: the window closed, its pause forgotten without a write, the hit map emptied, and the wiki's voices stopped with the close (`tagpu_wikisound_stop`); its DirectSound object and buffers stay for the process | game |
| `page_fill` after the demo's steps, and a press on the page (`tagpu_wiki.c` `sound_drain`, `button_sound`) | the simulation's sound events past the last played, and the page's `Options`, through `tagpu_wikisound.c`: a DirectSound object of the wiki's own (`DirectSoundCreate` from the `dsound.dll` the game imports, `DSSCL_PRIORITY` on `g_ddraw.hwnd`) and each WAV's four voices (a static buffer from the published WIKI3D's samples and three duplicates sharing them), made OFF the game thread -- on the 3D load's thread before the WIKI3D's publish, or, when the settings muted at the load's start, on a thread `tagpu_wikisound_arm` starts at a later open -- published by one interlocked store and kept for the process; on the game thread only `GetStatus`/`GetCurrentPosition`/`SetCurrentPosition`/`SetVolume`/`Play`/`Stop`, each play at −585; the close stops the voices | game |
| `tagpu_wiki_fill`, the first fill of an open window once the wiki's data is loaded, once a process | `assets_start`: the 3D asset load's inputs copied by value -- the mount list's paths (`tagpu_replays_archive_paths`, the list `running_mod` reads), the `language` value and the asset folders' names (`tagpu_replays_asset_names`), `GetCurrentDirectoryA`, the wiki's data and WIKIPAK -- and `tagpu_wikiasset_start`, which starts the loader below; nothing waits for it | game |
| the 3D asset loader's thread (`tagpu_wikiasset.c`, below-normal priority, started once under an interlocked compare) | the stage descriptions' TNT tiles and their hashes, their feature sections with their sequences or models, and every entry's unit types' FBI, 3DO and script, then every layout side's type for each cast the entry's units name, with Create()'s hides (`tagpu_cobscan_mask`, bits over `tagpu_model_walk` as the ghost's) and each script's bytes kept in the arena with its posed model's order and visible flags (`unit_script`, `tagpu_cobscan_posed`), and the game's own cursors (`<anims>\cursors.GAF`'s `cursornormal`, `cursormove` and `cursorreclamate`, WIKI3D `cursor`), and the units' sounds (`sound_load`: `<gamedata>\sound.TDF` and `allsound.TDF`, then every used category's and the button sounds' `sounds\<name>.WAV`, kept only as `tagpu_wav.c`'s PCM and copied into the arena, WIKI3D `sound`; and each unit type's death WAV, the `soundhit` of the weapon its ExplodeAs names, found as the FBI loader finds it, `death_load`, `WIKI3DUNIT.death_sound`), resolved by the engine's rules (`tagpu_vfs.c` over `tagpu_hpi.c` and `tagpu_tdf.c`, its own read-only handles) into one arena reserved `MEM_TOP_DOWN` and refused unless both ends pass `tagpu_m3_ptr_ok`; published by one `InterlockedExchangePointer` (`tagpu_wiki3d_get`), immutable and never freed, or, when the load fails, a refusal's sentinel by the same store (`tagpu_wiki3d_answer`: pending, ready or refused, as the sidebar's). **The sidebar first**, before the 3D load starts: `tagpu_wikiside_load` reads the running mod's sidebar (its layout, every side with MAIN2 and GEN, the pictures of every screen it can show, each side's PANELSIDE, the dots, the palette's tables, and each side's resource bars as the engine loads them once a process: frame 0 of PANELTOP and PANELBOT from its `intgaf` bank, `[SIDEn]`'s fifteen rects, `energycolor`, `metalcolor` and font, `<fonts>\<font>.<fnt_ext>` whole, the badge frames of `<textures>\<logos>.GAF`'s `<logos_seq>`, and the GUI colour table whole -- each value bounded as `gfx.py` bounds it, frames inside the sheet, rects ordered inside −1024..4096, at most 32 badge frames, every glyph's bits inside the font file; a side that fails one has no bar, with a report line; the bars' and badge's frames are registered after the dots so the sheet equals the pack's byte for byte, and left out when they would take it past 8192 rows) and each side's casting (the cast rule over the side's build menus and the unit files, with the display names in the copied `language`, each cast's FBI sight, radar and sonar words, `fbi_word`, and its model height, the 3DO's top by `height_chain` as `0x4CB5F0` takes it; `side_of_engine`, sidedata's index to the layout's side), the sight table of `<gamedata>\los.tdf` for the radius indices the casts and the players entries' types reach (`sight_read`, up to `CAP_LOS` 8 MB; none, with a report line, and Sight lights every block, when the file is missing or breaks a bound) by the engine's rules into one block, checked by `tagpu_wikilayout_check`, and `side_publish` stores the block, or the refusal's sentinel, by one `InterlockedExchangePointer`. ORDERING: the block is written whole before that store, a full barrier, and `tagpu_wikiside_get` reads the pointer with `InterlockedCompareExchangePointer`, so a reader that sees it sees every byte; the answer is final and the block never written or freed after it. A `CreateThread` that fails publishes the refusal, so the game thread never waits for an answer that cannot come. A read through `tagpu_hpi.c` is bounded on the range it asks for, `TAGPU_HPI_READMAX` (64 MiB), not on the file's size, so the sidebar's lookups into an 82 MB `fx.GAF` (Escalation) read only the pages they touch. It reads no engine memory and paints nothing; the log lines `wikiside: loaded in N ms - S screens, T types, R rows, a H-row sheet` and `wiki3d: loaded in N ms - arena B bytes ...` | the loader |
| `tagpu_wiki_carried`, every 30th fill | `tagpu_wikiasset_poll`: the trigger `tagpu_wiki3d.dump` writes the published load's report to `tagpu_wiki3d.json` and, when the sidebar answered `WIKISIDE_READY`, the sidebar's to `tagpu_wikiside.json` (each through its `.tmp`); it waits while the load runs | game |

**The units' scripts** run in the demo's `WIKISIM` on the game thread: `tagpu_wikicob.c`, on our
own COB VM `tagpu_cobvm.c`, with one host a simulation, its units' scripts copied from the
published 3D arena when the simulation is made (`tagpu_wiki.c` `scripts_now`, `cob_type`). No demo,
Try it or parity check starts while `tagpu_wiki3d_answer` is pending. The host reads and writes no
engine memory and no engine script object -- its units, their COB objects and its random number
generator are its own -- so it adds no field to the lists below, and neither do the poses it sets,
which reach the render thread only as the record's pose pool (§2.12d), nor the nano spray: its
particles are the simulation's own, written on the game thread into the record's `part` with the
view and read on the render thread only as the stage's ops draw them, after `view_bound`
(`tagpu_wikistate.c`) has held their count to the array; the engine addresses it names are
the facts it imitates ([the COB plan](../ingame-wiki-cob-plan.html), *The wiki host*). Its slice costs
about 70 µs over a demo's units; with the scripts on, the wiki's page cost the game thread at most
0.358 ms in a frame over 1222 frames, and `tagpu_wiki.check` played its 84 demos in 89 ms
(MEASURED 2026-10-28, a private Xvfb at 1920 × 1080). It compiles for SSE2 like the simulation.

**A game entry's replay** (`tagpu_wikireplay.c`, `tagpu_wikiframe.c`, `tagpu_wikibar.c`) is
engine-free like the simulation: it reads and writes no engine memory and names no engine address,
and its rules are the engine's, imitated (the economy, the wrecks and the death sound:
[the in-game wiki](../ingame-wiki.html), *A game*). The replay and its two simulations live on
the game thread; what crosses to the render thread is the frame record, by value in the packet,
bounded on the render thread's own copy before any read, and the stage HUD and the bars are that
thread's own statics.

**A live entry's replay** is the same layer's live mode (`tagpu_wikireplay.c`, LIVE in
`tagpu_wikireplay.h`), the game's live rewind state for state with its waits removed
([the in-game wiki](../ingame-wiki.html), *A live entry: the rewind on a live skirmish*). It holds
two simulations at most, the live one L and the past one P, and is the only code that frees them
(THE LIFETIME): P at a backward seek before the new one is made, at Back to now and at the end; L at
a fork, the moment P takes its place, and at the end. L is never stepped or written while the past
is shown, so Back to now shows its world exactly as it was. P re-simulates the selected timeline's
chain from tick 0, applying at each member's first tick its fork operation and between two ticks
the entries the member logged, through the calls that applied them live. **The fork rules are the
recorder's own code**, from `tagpu/ddraw/inc/tagpu_recrules.h`, which `tagpu_wikireplay.h` and
`tagpu_wikidata.h` include: Play from here's start is the last tick the writer flags a refresh
(`reclossy_is_refresh` at `REC_REFRESH_TICKS`), and the fork a new one evicts is `rec_fork_evict`'s
choice (at most `REC_FORK_LIMIT` live forks, never the original or the selected one, the least
recently used, the earliest on a tie). **THE BOUND**: a step runs at most `WIKI_REPLAY_SEEK` game
ticks and one fork operation per chain member P crosses; the timelines are at most
`RECR_TIMELINES` rows, the plan refusing the one past it, and the loader bounds a fork part's id by
the same constant; the logs hold at most `WIKI_REPLAY_LOG` entries over every kept log, a deleted
timeline's part and log freed once no live timeline's chain reads it, with `WIKI_REPLAY_LOG_KEEP`
held back so that the open and Back to now can always log what they let go. **The release**
(`tagpu_wikisim_release`), at Back to now and in every fork, lets the human seat's held state go
without acting, empties the selection and the armed mode, and drops every trip input not yet fed,
as the game's rebuild leaves the player (MEASURED 2026-11-01, [the replay
plan](../ingame-wiki-replay-plan.html), *What the game's live rewind leaves*); it runs only between
two ticks, and the loader's live data rule (`live_orders`) puts each build's and each reclaim's
select on the order's own tick, so it can never split them, and refuses a pick, whose popup a
release closes. **The view by mode** (`tagpu_wikiframe_view`): `WIKIFRAME_WATCH`
for a replay, the watcher's; `WIKIFRAME_PLAYER` while the rewind is idle or open, the live
simulation's view as it made it, the armed ghost only while the simulation's cursor is the one
drawn; `WIKIFRAME_PAST` while the past is shown, P's world and fog under L's column with nothing
selected or armed. The top bar's memo is DrawGameScreen's rule repeated, not read: its easing,
clamp and once-a-30-ticks rates ([render.md](../engine/render.html), *The memo's layout*), f32.
None of it reads or writes engine memory. Rewind's costs in the game: [the replay
plan](../ingame-wiki-replay-plan.html), *The measured costs*.

**A takeover entry's replay** is the same layer's Take over (`tagpu_wikireplay.c`, TAKE OVER in
`tagpu_wikireplay.h`; [the in-game wiki](../ingame-wiki.html), *A takeover entry: Take over a
replay*). **Every seat has its own input state** in the simulation (`tagpu_wikisim.h`, THE INPUT
SEATS: a takeover entry's game every seat of its scene; THE ORDERING: the acting seat `act` is 0
between calls, and every input rule reads `in[act]`, an index the loader bounds below
`WIKI_SEATS`); the column stays seat 0's, so a seat other than 0 has inputs only in a game whose
sidebar is false, and a takeover entry's games have none. **Watch mode** plays the selected
timeline's chain, the original or a fork the file holds, each member's fork operation applied as
the game reaches its R; the picker plans with the recorder's own rules (`tagpu_recrules.h`, as Play
from here) and snaps the playback to R − 1; a lane switches the selected timeline, keeping the game
when the chain members it has crossed are the same on both chains and re-simulating it otherwise;
a row of the open picker forks the game at R as that seat (`tagpu_wikisim_fork`: the taken seat's
recorded future and every later kill dropped, every other seat's inputs carried, the taken seat
alone released) and turns the replay into the live mode above with that seat as the human.
**THE BOUND, WATCH**: a switch restarts the walker in the background, which takes only what the
game's part of a step left of `WIKI_REPLAY_WALK`, so a step still runs at most max(SEEK, WALK, 8)
game ticks plus one fork operation per chain member crossed; the timelines are at most
`RECR_TIMELINES`, the forks saved at most as many, each once. **THE LIFETIME, WATCH**: a sought
switch frees the game before the new one is made, a switch frees the walker before the new one is
made, and the takeover frees the walker; a kept fork's part points into the entry's data, only
read. **Across the threads** the lanes the stage HUD drew go back by value in the command record
(`nlane`, `lane_id`), written with the run and `rep_serial` in one `tagpu_wikistate_drawn`, bounded
by `tagpu_wiki_take` and kept only while each names a live fork of the running replay
(`drawn_lanes`): no lock, handshake or lifetime of their own. None of it reads or writes engine
memory. Its costs in the game: [the replay plan](../ingame-wiki-replay-plan.html), *The measured
costs*.

**A players entry's replay** is the same layer's players state (`tagpu_wikireplay.c`, PLAYERS in
`tagpu_wikireplay.h`; [the in-game wiki](../ingame-wiki.html), *A players entry: Players and the
camera*), zero and unhashed in every other replay. Each step's tail, after the game's part, moves
the camera (Jump once, Follow every step, onto the seat's recorded camera, `tagpu_wikidata_seat_cam`;
nothing else moves it), applies a Sight request with the game's `bar_rates_due` rule copied (held
while a seek is pending; a seek's end sets every DisplayTimer to the shown tick and the click ring's
start), runs the pass at a Sight change and a seek's end -- the viewed seat's lit grid as fog bits
and the units its contacts mark (`tagpu_wikisim_sight_lit`, `tagpu_wikisim_contacts`) -- and eases
the top bar's memo; the game's part runs the pass on every played tick that reaches the viewed
record's 30-tick deadline, at 2× to 8× as at 1×. The teams chip acts only while the teams are told
apart, as the game's HUD enables it. The walker samples every seat's seven
measures every 30 game ticks from tick 0, at most `WIKI_STAT_PTS` (181), read by
`tagpu_wikireplay_stat`. The Players set, the folds and the chips act only in a players replay; a
seat, chip or row outside the scene is refused at `tagpu_wikireplay_press`'s door. **Across the
threads** it adds values only: the record's players block and the view's eye, overlays and minimap
dots go to the render thread in the packet, bounded there (`tagpu_wikiframe_bound`,
`view_bound`), and the image's rect and clip come back in the command record for the dump; no
lock, handshake or lifetime of its own. None of it reads or writes engine memory: the engine facts
it imitates -- the sight table and the stamp, the temporary sight records, the radar pass's contact
tests, the eye clamp and the minimap's camera box -- are in the engine map's
[map.md](../engine/map.html), [recorder.md](../engine/recorder.html) and
[input.md](../engine/input.html).

Watching's costs (MEASURED 2026-10-31 on the reference setup, the DLL's i686 build under Wine: the
`watch` lane at 1920 × 1080 for the page, the HUD and the pass; `tagpu_wiki.check` under every
setup of the compatibility suite for the ticks): Watching's page cost the game thread at most 0.462 ms in a frame over 174 frames; the
stage HUD's two builds at most 0.159 ms on the render thread, the bars 0.301 ms in a frame that
composes them and nothing in one that keeps them; the wiki's layer peaked at 1 309 of 32 768
instances and 87 batches of 1024; a replay tick costs 1.5 to 3.8 µs on every setup of the
compatibility suite but Escalation (11.6 to 15.3 µs), whose dearest 64-tick step is 3.57 ms
against 0.41 ms or less elsewhere ([the replay plan](../ingame-wiki-replay-plan.html), *The
measured costs*).

**The sound** crosses no thread. Its WAVs are read on the 3D load's thread and reach the game
thread only inside the published WIKI3D, immutable and never freed, so a buffer made from a clip
never outlives its samples; the events are the simulation's, made and played on the game thread,
and the render thread never sees them. The mixer is game thread only, and the engine's own sound
object `*(main+0x10)` is never touched ([the COB plan](../ingame-wiki-cob-plan.html), *Sound*).

**Fields we write**, on the game thread, through `tagpu_menu_pause_hold`: bit 0 of `main+0x38A51`
(the pause), only when `main+0x2A44` bit 0 is clear (a single-player level), set at the open only
when it was clear and cleared at the close only when the open set it. The window does not open
while one of Impure's three writers that put a pause back, or end one, later and without input
has its write in flight (`pause_in_flight`, `tagpu_wiki.c`): a rewind not idle, Leave's question,
or a takeover queued, whose run ends the playing's pause (`tk_run`, `tagpu_replay.c`); each
refusal is logged `wiki: not opened - ...`. Of our own: the game
folder's `tagpu_wiki.json`, `tagpu_wiki3d.json`, `tagpu_wikiside.json` and `tagpu_wiki.layout.json`
(the layout a `tagpu_wiki.check` played on; each through its `.tmp`) and the deletion of
`tagpu_wiki.dump`, `tagpu_wiki3d.dump`, `tagpu_wiki.check`, `tagpu_wiki.shot` and `tagpu_wiki.pause`, and the
reads of `tagpu_wiki.turbo` (game thread); and, on the render thread, `tagpu_wiki3d_shot.ppm` -- unlinked when a
`tagpu_wiki.shot` capture is armed and written by `tagpu_vk_shot_finish` -- the reads of the lever
`tagpu_wiki3d.on`, and the read and deletion of `tagpu_wiki3d.count`. **Fields we read**: `main+0x2A44`
bit 0 and `main+0x38A51` bit 0; through `tagpu_menu_sound`, on the game thread at each wiki sound, at the asset load's start and at each open,
the player's sound settings -- the dword `main+0x37F0C` (fxvol), the bytes `main+0x37F17`
(unitchat) and `main+0x37F19` (`& 7` Sound Mode, bit `0x40` speechfx), and the dwords `0x51E690`
(NoDirectSound) and `0x51E694` (UseWindowsSound) -- plain reads of values the engine's settings
code writes (the registry load `0x42F9A0`, `0x45BE3B..0x45FEEE` [INFERRED: the options screens], the sound init `0x47ED40` and the
flag setters `0x47EFC0` and `0x47EFD0`), nothing written (the owner's decision of 2026-10-28 on
`thread-split.allow`'s `tagpu_menu.c` line); at every packet fill, in `tagpu_packet_pub.c` (the wiki's own files read no engine memory), byte `main+0x2A42` (held below main's eleven player records), the record's PlayerInfo pointer at `main+0x1B63 + 0x14B·p + 0x27` (`ptr_ok` as a sanity filter) and its byte `+0x95`, and the int `main+0x37EFA`, each passed to `tagpu_wiki_fill` by value, the side byte as data the wiki maps through the published block (engine map, *The wiki's per-mod tutorials*); and, once, at the asset load's start, through `tagpu_replays.c`:
the mount list (the graphics globals `*0x51FBD0`'s `+0x618` bounded by `+0x61C`, 0x104 bytes a
path), the `language` value `0x51FB50` and the folder names in `.data` (`0x503920`, `0x503720`,
`0x504734`, `0x503704`, `0x5036E4`, `0x503740`, `0x502E30`, `0x502C7C`, and for the sidebar
`guis` `0x502820`, `gamedata` `0x503360`, `unitpics` `0x505FE4`, `download` `0x503730`,
`commongui` `0x50925C`, `palettes` `0x5031BC`, `PALETTE` `0x5033A4` and `guipal` `0x5031C8`, and for the game frame's bars `fonts` `0x50338C`, its extension `FNT` `0x503394` and the badge's sequence `32xlogos` `0x504728`),
each bounded to its buffer. They are read in the running process because a mod's loader DLL
rewrites some of them in `DllMain` (Mayhem's `guiM`, ProTA's `guiP`; the engine map's *The left
sidebar*, *The folder names a mod renames*). **No new engine hook, patch or detour**: the sidebar
is drawn from those files alone, and its rules are the engine's repeated in `tagpu_wikisim.c`. The rules the loader repeats: [engine map](../exe-reverse-engineering.html), *How the engine
resolves a path, a unit's files and a model's textures*. Nothing the recording carries: the demo's simulation is the wiki's own, a `WIKISIM`
on the game thread that TA's simulation never reads, and every wiki file is `presentation`.

### 2.12d The wiki's stage in 3D: the world passes compiled twice (`tagpu_scene.h`, `tagpu_wikiworld.c`, `tagpu_wikiscene.c`, `tagpu_scene_wiki.h`, `tools/scene-check.sh`; every Vulkan level with the wiki open)

The eight files that draw one world scene — `tagpu_vk_world.c`, `tagpu_vk_fog.c`,
`tagpu_vk_terr.c`, `tagpu_vk_feat.c`, `tagpu_vk_unit.c`, `tagpu_posedraw.c`, `tagpu_posebake.c` and
`tagpu_render3do.c` — are compiled **twice**: `src/<name>.o` is the game's world, and
`src/<name>.wiki.o`, built with `-DTAGPU_SCENE_WIKI`, is the in-game wiki's stage, each with its
own statics. Both are linked. Design: [the 3D stage plan](../ingame-wiki-3d-stage-plan.html),
section 1 and landings 6f, 6g and 6h. **No engine address, no hook, no byte patch**: nothing for
the tables above; the one engine value the stage reads is the graphics-option word, off the
frame's packet (`gfx_opt`). The wiki's copies of the world target, the terrain and the features
draw the stage's ground; its copies of the unit pass, the pose bake, the pose draw and the 3DO
renderer draw its units and ghosts.

- **What `tagpu_scene.h` does under the macro**, included first by each of the eight: renames
  every external symbol they define (`tagpu_vk_*` → `tagpu_vkw_*`, `tagpu_r3d_*` → `tagpu_r3dw_*`,
  `tagpu_posedraw_*` → `tagpu_pdw_*`, `tagpu_posebake_*` → `tagpu_pbw_*`); points their hand-over
  sources (`tagpu_terr_handover`, `tagpu_feat_handover`, `tagpu_feat_atlas_hand/owed/ack`,
  `tagpu_native_worldtgt`, `tagpu_native_scissor_on`) at `tagpu_wikiworld_*`; points every call into a game-only module (the cast-shadow map, the
  scaffold, the effects pass, the Classic++ restorer, `tagpu_classicpp_assets`/`_light`, the fault
  lever, `tagpu_vk_ab_arm`, `tagpu_grow_stress`) at an inert answer in `tagpu_wikiworld.c`; and
  sizes the bake's caches, the unit pass's vertex-buffer table and their lookup hashes from the
  stage's bound, `WIKIWORLD_FRAMEMAX` = 128 units + 256 gesture-site ghosts + the placement ghost +
  64 3D features = 449 (the keeps at the bound too: `tagpu_posebake.h` asserts that no keep exceeds
  its table, which would undo the bound). Without the macro it does nothing.
- **The sources the wiki's copies read.** `tagpu_wikiworld.c` defines them with
  `TAGPU_SCENE_WIKI` set, so each definition is checked against the very prototype the wiki copy
  calls. The terrain, feature and target sources answer from its producer (below); every other one
  answers "nothing this frame" (0, a null view, `VK_FORMAT_UNDEFINED` for the shadow format, the
  restorer down), each the answer the passes already take from the game's own module on a frame
  with nothing to give.
- **The producer**, `tagpu_wikiworld_frame`, once a frame on the render thread in
  `tagpu_overlay_draw` after `tagpu_wikistate_present`: from the stage this frame draws
  (`tagpu_wikistate_scene`: the record's view, bounded, the entry, the stage's rect) and the
  arena's stage of that entry (`tagpu_wiki3d_get`, matched by id; a stage that did not resolve, or
  whose map is not the view's, is the whitebox's), it builds once a stage the scene
  (`tagpu_wikiscene_ground`: every cell's tile, the deposits' and vents' quads with
  `tagpu_feat.c`'s depth bands), the stage's tile map (each 32-px cell's tile index, row by row,
  what the terrain's vertex stage draws from; a ground whose cells are not exactly its tile grid is
  refused: `the ground's cells are not its tile grid`), the R8 terrain atlas in `tagpu_terr.c`'s
  layout (64 tiles a row, 34-texel pitch, a one-texel replicated border) from the arena's tiles,
  and the features' frames
  in an atlas of its own (`TAGPU_GAFATLAS`, 512 square, 128 entries, a key plane, no allowance),
  painted then, with every quad's six vertices as `emit_frame` makes them (the edge nudge, each
  corner's own world point); and every frame the fog source (`tagpu_wikiscene_fog`: one cell a
  32-unit block, bit 0 the view's black, bit 1 its grey, in `inc/tagpu_fogsrc.h`'s format; the
  seam prepares the stage's own fog image from it, `tagpu_vkw_fog_prepare(…,
  tagpu_wikiworld_fogsrc())`, its serial moved only when the bits change) and three
  hand-overs: the terrain's (`gw` = map cells × 16, zoom 1, the eye at the map's corner, the
  viewport the whole map, the grid the whole tile map and the area the same, mirror and
  neighbourhood keys off, the scissor on, no relief, restore or shadow; the tile map's serial moves
  with each stage built, so the Vulkan lane uploads it once a stage), the features' (the same
  camera), and the target's (`TAGPU_WORLDTGT`: `gw` × `gh` at `tagpu_settings_ss`, composited at
  `tagpu_wikistage_image`'s rect on the attachment -- the stage fit's map rect, each edge snapped
  to the device pixel as `stage.js` snaps it). The lever `tagpu_wiki3d.on`, read every 30 frames:
  `off` (no stage in 3D), `nomap`, `nofeat` (that hand-over withheld while still expected, so the
  frame shows the whitebox whole), `nounits` (no unit drawn in 3D: every type the whitebox's box)
  and up to eight `nounit=TYPE` (that type the box alone).
- **The frame count**, `tagpu_wiki3d.count` (N, 1 to 3000), read on the lever's beat right after it,
  so a count asked once a lever's line is out counts frames under that lever: the stage's next N
  drawn frames, from the first drawn frame after the ask that is *at rest* -- no ground built
  (`ground_build`), no type records built (`types_build`), nothing baked or dropped by the wiki's
  bake (`tagpu_posebake_work`: its entries baked and dropped, both tables, each count only growing)
  since the drawn frame before it, and no record the bake refused -- said in one line
  (`wiki3d: count K: D of N frame(s) drew the image, frames A..B; the work: ...; render thread: ...`)
  with the work done over the window and the producer's and the passes' milliseconds. A stage that
  works on every frame never comes to rest and the count goes unanswered. The periodic line, every
  300 frames, gives the same image, record and time numbers without the work, for people reading
  the log; the harness reads the count.
- **The units**, every frame from the same view, through `tagpu_wikiscene_models`
  (`tagpu_wikiscene.c`, equal record for record to `scene.js`: `scenecheck.mjs`): one record a
  unit of a type the arena has (its owner, the record's yaw `(cs, sn) = (-hz, hx)` from the facing,
  a structure's always `(0, 1)`, the depth row's key, the nano staging of `tagpu_nanostage.h` while
  `built < 1` and then no shadow), then the placement ghost, the gesture's sites and the queued
  sites not yet standing, alpha 0.40, no shadow. The arena's types are matched to the stage's by
  name once a stage (`types_build`), each type's node run walked with `tagpu_model_walk` and every
  node checked to lie inside the arena **before** it is handed over; a type that fails is drawn as
  the box (`resolved`, bit t). Each record goes to the wiki bake as a piece run
  (`tagpu_pbw_unit`, key `ghost` 1, so units and ghosts of a type share one geometry). **A unit
  its script poses** takes the pose the record's pool carries (`pose_of`, `pose_compose`):
  `tagpu_native.c` `posed_pose`'s composition, P = parent · T(rest offset from the parent + MOVE) ·
  R(TURN) down the tree, through `tagpu_wikiasset.c`'s `posed_of` map from the stage's walk to the
  posed order; at the root the facing below stands for the body turn, about the root's rest point,
  with the root's MOVE outside it; a node the posed order does not hold, or whose parent never
  resolves, keeps the rest pose; the flags give `posed_pose`'s words (hidden: an all-zero matrix
  and visibility 0; visible and cached 3; dont-cache 1; shaded by bit 2 once any visible piece
  carries it). `rot2`, `m43_mul` and `piece_local` are `tagpu_posemath.h`'s, which
  `tagpu_native.c` includes too, so the game's units and the stage's are posed by one arithmetic.
  **A ghost, and a unit no script poses or whose run `pose_of` refuses,** is posed at rest: one
  rotation about the root's rest point, `(cs, 0, sn; 0, 1, 0; -sn, 0, cs)` on the engine-held
  nodes, which is the engine's body turn of the matching heading (`0x4CB590` in the engine map); a
  piece `Create()` hides takes visibility 0, one it marks dont-cache 1. The camera is
  the map's corner at zoom 1 (`depthScale` 512, the scaffold rect the whole map), the light the
  Classic one. From the packet's option word: the shadow kind (bit 2 Shadow: a structure's slant
  when its type has one; bit 3 as well: a mobile unit's silhouette; never for a nanoframe or a
  ghost) and the lit rows (bit 5 Shading: a structure, and every ghost). The calls in their order a
  frame: `tagpu_r3dw_atlas_level(1)` (a constant level generation), the atlas recycle,
  `tagpu_pbw_frame`, `tagpu_pdw_frame`, the 3DO renderer's ensure and atlas frame (the allowance
  off from the first frame), the shade table, then the records; a frame on which the bake refused
  any record hands nothing over, and the frame shows the whitebox whole.
- **In the seam** (`vk_present`, `tagpu_vk.c`): after the game's prepares and before
  `tagpu_vk_hud_prepare`, the wiki's world target is prepared first (its own extent), then its
  terrain, its features, and its units (the upload always, so a frame the allowance withheld still
  moves the atlas on; the prepare only for a hand-over that carries every record); **the image is
  drawn whole or not at all** -- only when the target and
  every pass the producer said the frame expects (`tagpu_wikiworld_wants`) prepared on this frame
  -- and `tagpu_wikistate_image` splits the stage's ops for that answer before the HUD's pass
  builds them (`tagpu_wikistage_keep`: R0 under the image, R2 over it; R0 and R1 for the
  whitebox), and `tagpu_wikistate_image` is told which types the image drew in 3D, whose boxes it
  then leaves out. After the game's world render pass, the wiki's own (begin, terrain, features,
  units, ghosts, end);
  in the frame's pass, between `tagpu_vk_hud_record_wiki` and `tagpu_vk_hud_record_wiki_over`,
  the wiki's `tagpu_vkw_world_record` composites the target, LINEAR, under the Gamma curve. Its
  teardown is the game's: `tagpu_vkw_*_down` in `vk_down` and `passes_down` behind their
  `vkDeviceWaitIdle`, and the four passes' (terrain, features, units, world) `_down_owed` in the owed list, paid behind the same
  wait. The trigger `tagpu_wiki.shot` (`ENTRY DEMO TICK`: the demo played to the tick and paused on
  the game thread, the record numbering the capture) is taken through the A/B's one capture buffer
  on the first frame of that view whose image was drawn, and only while no capture is in flight
  (`s_abSlot1` 0), into `tagpu_wiki3d_shot.ppm`: the wiki's target before the Gamma curve.
- **What it draws**, MEASURED 2026-10-27 on the reference setup (retail, a private Xvfb, the RTX
  presenting): quickbuild's ground, 2 tiles and 240 cells, 11 feature quads, a 2176 × 34 terrain
  atlas and 2 feature frames; the target 1280 × 768 at ss 2 and 640 × 384 at ss 1. The render
  thread's time for the stage, from its periodic line: the first frame 4.9 ms in the seam (the
  targets built, both atlases sent), then the producer 0.06 ms and the passes 0.02 ms a frame on
  average, at most 0.04 ms, at 1920 × 1080 and at 3840 × 2160 alike. Against the mockup
  (`tools/wiki/stage3dcheck.py` over `wiki_regression.py stage3d`'s 18 captures a run, three demos
  at three ticks at ss 2 and 1): every pixel equal, terrain and features, in both runs. Under
  `nomap` and `nofeat`, 0 of 300 frames drew the image; with the lever gone, 300 of 300.
- **The invariants.** *No static of the wiki's stage is a static of the game's world*: two
  translation units share no file-scope state, which C's linkage guarantees for every static
  these files hold and every one a later edit adds. The one deliberate exception is the bake's
  scratch (`tagpu_pb_scratch_*`: the two vertex streams, the skip flags and the normal sums,
  5.3 MB), defined by the game's object and `extern` in the wiki's (`PB_SCRATCH`); it holds one
  model during one `tagpu_posebake_unit` call, both copies run on the render thread only, and
  neither calls the other. *Every external symbol is renamed*: a missed one is defined twice and
  the link fails naming it (checked by planting one). *No wiki copy reaches a game-only module*:
  `tools/scene-check.sh`, a prerequisite of the link that depends on every object, fails on any
  undefined symbol of a wiki object that another object of ours defines and its allow list does
  not name (stateless or read-only helpers: `tagpu_vk_stage_*`, `tagpu_gaf_atlas_*`/`_frame_*`/
  `_seq_*`/`_rects_*`, four `tagpu_pal_*` reads, the device queries, `tagpu_log`/`_logf`,
  `tagpu_settings_ss`, `tagpu_user_top_bound`). MEASURED 2026-10-26 by planting a call to
  `tagpu_vk_shadow_prepare` in a wiki copy: `scene-check: src/tagpu_vk_world.wiki.o calls
  tagpu_vk_shadow_prepare, which a module of the game's world defines`.
- **The game's path is unchanged.** MEASURED 2026-10-26: six of the seven game objects are
  byte-identical to the base commit's; `tagpu_posebake.o` differs only in that its scratch has
  external linkage, which moves `.bss` offsets and one register choice. On `tascene-parity`,
  paused, the base commit's DLL and this one give byte-identical terrain, feature and unit
  captures (`tacli ab`, each pass armed alone), and a presented window under the whole play set
  that differs by 0 px below the message band (whose "forces obliterated" wording is random per
  boot), the cursor's pulsing hotspot pixel aside.
- **What it costs**, MEASURED 2026-10-26 with `size` and `objdump -h` on the i686 build:
  `ddraw.dll` 17 778 688 → 18 040 832 bytes (+262 144: `.text` +115 424, `.rdata` +126 592, the
  shaders' SPIR-V arrays included twice); `.bss` +363 296. The wiki objects' `.bss`:
  `tagpu_render3do` 188 928 (its 2 048-entry atlas table, not resized), `tagpu_posebake` 70 464,
  `tagpu_vk_unit` 63 168, `tagpu_vk_feat` 34 400, `tagpu_posedraw` 2 656, `tagpu_vk_terr` 2 304,
  `tagpu_vk_world` 1 408. The stock-limits build's are the same.
- **The invariants.** *A hand-over lives one iteration of the render loop*: the producer runs in
  `tagpu_overlay_draw` and the seam takes the hand-overs later in the same iteration on the same
  thread (`render_vk.c`: `tagpu_overlay_draw`, `tagpu_packet_frame_end`, `tagpu_vk_frame`), each
  stamped with the frame and refused on any other, taken once; every pointer in one names a buffer
  of `tagpu_wikiworld.c` -- rebuilt only by the producer, only at a change of stage, so never
  between a publish and its take -- or the arena, immutable and never freed. *The feature atlas is
  painted on the render thread only*, because `tagpu_gaf.c` decodes through static buffers that
  the game's atlases, painted on the same thread, share. *No device object of the wiki's passes is
  freed while a submit may name it*: their teardown is the seam's, behind its `vkDeviceWaitIdle`.
- **What the units cost and how they compare**, MEASURED 2026-10-27 (`wiki_regression.py units`,
  the reference setup, retail, a private Xvfb with the RTX presenting): the arena 249 124 bytes,
  loaded in 42 to 56 ms, 9 of 9 unit types resolved; quickbuild's types ARMCK ARMACK ARMMEX
  ARMMOHO ARMGEO ARMPW, no box. The render thread's time with the units, from the periodic line:
  the first frame about 10 ms producer and 8 ms passes (the bake and the atlas), then the producer
  0.067 ms and the passes 0.037 ms a frame on average, the passes at most 0.057 ms, at 1920 × 1080,
  and 0.070, 0.041 and 0.062 ms at 3840 × 2160; 0 frames with a refused record, 300 of 300 frames
  drew the image. Against the mockup, ten captures of Quick Build 3, 8
  and 9 and Build Lines 2 and 4 at ss 2: every terrain pixel equal, every feature pixel equal, and
  over the unit pixels a mean difference of 0.03 to 0.38 / 255 a channel with 0.003 to 0.19 % of
  them past 40 (the tolerance: 2 / 255 and 0.5 %). Under `nounit=ARMCK` the types line names
  ARMCK alone among the boxes and 300 of 300 frames still drew the image.
- **The invariants at the units' hand-over.** *The wiki bake reads the arena only, never game
  memory*: the loader wrote every pointer in the arena, each inside it, and copies no word of a
  file that a reader follows as a pointer (a face's texture state past its name starts at 0 and
  only the binder writes it). `types_build` then re-checks every node of a type's run against the
  arena's used extent (`WIKI3D.base`, `bytes`) on its value before the run is kept, so a loader
  that broke that rule makes a box. *The arena outlives every hand-over*: it is immutable and never freed. *The
  pose is a function of the record*: the rest pose, the record's yaw and the view's pose pool,
  nothing read from a unit of the game. *The pool is data*: the game thread writes it into the
  record and the render thread reads it from the packet's copy, so `pose_of` bounds every number
  of it before use -- the slot against the view's `nunit` and `WIKISTAGE_UNITS`, `npose` against
  `WIKISTAGE_POSE`, `pose_at` and `pose_n` inside `npose`, `pose_n` equal to the type's primitive
  count and the type's run the `nwalk` nodes `posed_of` was made against -- and `pose_compose`
  bounds each `posed_of` entry by that count; a run that fails any of them draws the rest pose.
- **The nano spray is not a world pass's.** The wiki's copies compile without the effects pass
  (`tagpu_fx.c`, `tagpu_sfx.c` and `tagpu_vk_fx.c` are not among the seven, and the fx models'
  answer is the inert one), so the stage's particles -- the wiki simulation's own, in the record's
  `part` (§2.12c) -- are drawn as 2D dots by the stage's ops in the range over the image, 2 × 2
  game pixels each, after the units; they take no depth and no fog shader, only the stage's own
  fog-cell test.
- **3D features** (a wreck, a heap) are records of the unit pass, after the units in the opaque
  window: `tagpu_wikiworld.c` `wrecks_build` walks each arena feature's model once a stage, every
  node checked inside the arena as a type's run is, and `unit_one` draws it with no pose, nothing
  hidden and `lit` 0, at the record `tagpu_wikiscene_models` makes ([the in-game wiki](../ingame-wiki.html)).
  A walk refused falls the ground back whole.
- **Not done here**: the wiki's device memory is computed from the sizes it
  creates, not read back (the plan's section 1); the GDI lane draws no wiki at all; the poses'
  and the spray's cost -- the 80 KB pool and the 16 KB of particles in the record at every fill
  that shows the stage, every posed unit composed every frame -- is not measured.
