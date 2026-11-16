# The drawing board (the plan)

## Status

**Decided with the owner 2026-11-15 on an interactive HTML proposal. Landings 1 to 5 — the local
board, its network channel, the allies' pointers, finding marks and the settings — are built and
verified on the branch ([board](modules/board.html)); landing 6 is not.** Allies
draw on the map together: freehand lines, markers with or without text that anyone on the team can
edit, move or delete, and an eraser. Each ally's cursor is shown live. Worked on
`worktree-drawing_board`.

TADR's whiteboard (`vendor/TADR/src/DDraw/whiteboard.cpp`, `maprect.cpp`, `PlayerMute.h` at TADR
`dcff5dd`) and BAR's map marks (the Recoil engine's `InMapDraw*` and `GameServer.cpp`, BAR's
`gui_chat.lua`, `gui_point_tracker.lua`, `gui_ally_cursors.lua`, `map_auto_mapmark_eraser.lua`)
were read as guides only. Every fact the code rests on comes from our own disassembly and source.

## What it does

| Feature | Behaviour | From |
|---|---|---|
| Draw key | Hold `\`, rebindable. It replaces the stock repeat-last-command (below) | TADR, BAR |
| Lines | Left-drag; drawn on the ground in the sender's colour with a dark edge | TADR, BAR |
| Markers | Middle-click for a plain marker; double-click for one with text, up to 60 characters | TADR, BAR |
| Edit and move | Double-click a marker to change its text, drag it to move it; anyone on the team | TADR |
| Erase | Right-drag erases within 48 world units, any part of a line. Right double-click deletes one marker | TADR, BAR |
| A new marker arrives | A chat line `Name: text` and a two-second ping on the minimap | TADR, BAR |
| Off screen | An arrow at the screen edge for 10 s in the sender's colour; a click jumps to it | BAR |
| Last marker | `Ctrl`+`\` jumps to the last one received; each ally's controls jump to theirs | TADR, BAR |
| Ally cursors | An arrow with the ally's name in the world, a dot on the minimap | BAR, new |
| Activity | *drawing* or *erasing* beside an ally's name while they do it | BAR |
| Mute | Per ally, on the local player's screen only: cursor, markers, lines. A muted ally's erase removes only their own marks | TADR, new |
| Lifetime | Markers and lines fade after 60 s; an option keeps them until erased | BAR, TADR |
| Sender | Every message names its seat | BAR |
| Spam limit | A new marker's chat line at most once a seat each 30 ticks; the marks are bounded by the store, the body and the channel's queue rather than by a rate. BAR's rule (a player's marks dropped once more than 25 arrive less than 50 ms apart) would drop an ordinary fast stroke here, which is sent once a frame | BAR, changed |
| Watchers | See every player's marks and cursors; their own marks reach only other watchers | BAR |
| Replays | Marks are recorded; the replay HUD gets a Marks switch beside Cursors, Clicks, Selections and Chat | BAR, Impure |

## The owner's decisions

| # | Question | Decision |
|---|---|---|
| 1 | Allies' view boxes | **None.** No view boxes on the minimap and no button to move to or follow an ally's camera. Cursors only |
| 2 | Who receives marks | **Every peer**, shown only to allies and watchers, so every peer's recording holds them |
| 3 | Cameras and cursors | **Keep going to every peer** for the recorder, as now; a modified client could read them |
| 4 | Default lifetime | **Fade after 60 s**; *until erased* is an option |
| 5 | The key | **`\`, rebindable** |
| 6 | Following an ally live | **No** |
| 7 | Where each ally's Mute and Last-marker controls live (2026-11-16, on a mockup of three) | **On the allies' ribbon**: two buttons on each ally's chip and the activity line under its bars |
| 8 | Rebinding the draw key (2026-11-16) | **A press-to-bind row** in the in-game gear panel: click it, press any key; `drawkey=` in `impure.cfg` |
| 9 | What the replay landing covers (2026-11-16) | **Multiplayer replays and the HUD's Marks switch**; marks in skirmish recordings are a later landing |

## What the engine and our code give us

Gathered 2026-11-15 from the source and the pristine exe; the scouts' citations are kept in the
landings' notes as each one uses them.

- **The contribution channel** runs in every network game between Impure peers, the recorder on or
  off: its one gate is the network mode with `main+0x2A44` bit 0 (`tagpu_recorder.c`, the call of
  `mp_capture`); the recorder setting only decides whether a file is written. A game with a peer
  without Impure has no channel, and refuses that peer at the join (`mp_join_admit`). A skirmish
  has none either.
- **A send** goes through `tagpu_recmp_send_from` at the current capture tick `s_mp_tick`, on the
  game thread, at most `RECMP_BODY_MAX` (4032) bytes, into a 256-slot queue; every refusal fails
  the whole channel, so a sender checks `tagpu_recmp_room` first.
- **A receipt** is taken only by `mp_apply`, at the capture point B2 of each simulation tick, in
  delivery order. **A paused game runs no B2**, so today nothing is taken and no camera sample is
  sent while paused. An unknown kind fails an older peer's whole channel, so the new kind raises
  `RECMP_START_VERSION`.
- **Camera and cursor samples** (`RECMP_POV`, every 6 ticks, `tagpu_recpov.h`) already reach every
  peer and sit in the cache `s_mp_pov`, two samples per origin. Nothing shows them in a live game.
- **Input.** The in-play mouse handler `0x499200` never sees the middle button (the window procedure
  dispatches `0x200..0x206` only, `0x4B5E3B..0x4B5E4A`), and the engine's click routing ignores
  double clicks. A left drag in mode 1 can be taken only at the stub on `0x49939B`, which the
  formation move already uses (`tagpu_route.c`). The stock `\` is `WM_CHAR` `0x5C`: the in-play key
  table sends it through byte `0x4966E7` to `0x49637A`, which acts only in SoftwareDebugMode
  (`main+0x37F2F` bit 1) by calling the typed-command handler `0x417B50(0,-1)`.
- **Drawing.** The range pass's sensor outlines are the one ground-following line with width and a
  halo (`tagpu_vk_range.c`); the marker pass draws flat sprites and constant-size text in the
  engine font, glyphs `0x20..0x7E`, at most 24 draws a frame. Impure has no text-entry widget.
- **Chat.** `0x463CA0(text, flags, unit, row)` writes a chat line, 63 characters, in one of two
  fixed colours, with a sound unless the row is 10 (`tagpu_modgate.c`, `tagpu_replay.c` call it).
- **Allies.** `tagpu_share_allies` (`tagpu_sharetick.c`) returns the mutual allies with colour and
  name; a watcher is PlayerInfo `+0x9B` bit `0x40`.

## Design

- **One module, `tagpu_board.c`**, a publisher with presentation scope: the gestures, the mark
  store, its packet table and the channel's mark kind. It writes nothing the simulation reads.
- **The store** keeps markers by id (seat and a per-seat sequence) and line segments in a bounded
  ring, each with its owner seat and birth time. An edit, move or delete names its marker's id;
  one for an unknown id is dropped. Every received value is data: coordinates are bounded by the
  map, text by its length and the glyph range, seats by the roster.
- **The wire:** one contribution kind, `RECMP_MARK`, whose body is a batch of operations POINT,
  EDIT, MOVE, DELETE, LINE, ERASE. Sends are batched once per frame and skipped while
  `tagpu_recmp_room` is low; a skipped batch stays queued locally.
- **Marks while paused.** While the game is paused, the frame visit runs the same take loop at the
  current capture tick, so whatever has arrived is applied in delivery order as a B2 would. Sends
  already go at the current capture tick between two B2s. Whether every cache and the materializer
  accept packets taken while paused is checked in landing 2 before it relies on it. Cursors still
  freeze while paused, as the samples are taken at B2.
- **Drawing on Vulkan only.** Lines on the ground like the range pass's outlines; pins and labels in
  the marker pass; the edge arrows and the text box in the HUD layer. GDI draws none of it.

## Landings

1. **The local board.** The key (`WM_CHAR` `0x5C` taken from the window procedure, `0x4966E7`'s
   entry left alone), the gestures, the text box, the store, lines, markers and the eraser drawn
   on Vulkan. Works in a skirmish. Verified by injected input and captures. **Built.** What it
   left for later: the lifetime is wall time (marks fade while paused), and the key is the virtual
   key `VK_OEM_5`, not the character `\`.
2. **Allies receive it.** `RECMP_MARK` everywhere a kind is listed (`tagpu_recmp.c`'s two lists,
   the stream ledger, the journal, the history's `semantic()`, `mp_apply`,
   `tools/replay/recmp_file.py`, `recmp_cut_test.c`), `RECMP_START_VERSION` raised, the paused
   take, the ally and watcher filter, the chat line and the spam limit. Verified in a two-player
   network game, paused and running. **Built.** The minimap ping went to landing 4 with the
   minimap's marks; the lifetime is counted in game ticks, so paused marks no longer fade.
3. **Ally cursors**, from the POV cache: the arrow and name in the world and the dot on the
   minimap, drawn 200 ms behind and interpolated between the last two samples. **Built**, through
   the playback's perspective clock and the board's marker table.
4. **Finding marks.** Marks on the minimap and the new marker's ping there, the edge arrows, `Ctrl`+`\`, and each ally's controls
   (mute and last marker), on the allies' ribbon (decision 7). **Built.** An ally out of the game
   has no row: the ribbon dims its chip as one group, and groups do not nest.
5. **Settings.** Lifetime and the key in `impure.cfg` and the in-game settings. The lifetime is
   **built**, and so is the key's press-to-bind row (decision 8): a plain button whose caption is
   the key's name and no accelerator; Esc, Tab, Return, Backspace, the modifiers, the three lock
   keys, F10 and Print Screen cannot be the key.
6. **Replays.** The history keeps the marks so a seek restores them; the replay HUD's Marks switch.

The landing on main runs the replay gate, as the recorder's channel changes.

## Gaps

- Ally cursors freeze while the game is paused.
- Players without Impure cannot join an Impure game at all, so there is no mixed case.
