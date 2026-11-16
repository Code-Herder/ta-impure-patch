# Drawing board: lines and markers on the map

Hold **`\`** over the world view and draw on the ground:

| gesture (with `\` held) | does |
|---|---|
| left drag | a line on the ground, in the player's colour on a black edge |
| middle click | a marker: a pin in the player's colour |
| left double click | a text box: on a marker, its text; elsewhere, a new marker with text |
| left drag on a marker | moves it |
| right drag | erases every line segment and marker within 48 world px of the path |
| right double click on a marker | deletes it; a single right click on a pin erases nothing where it stands |

In the text box, the engine font's glyphs `0x20..0x7E` type, up to 60; Backspace deletes, Return
keeps the text and Esc drops it; a click anywhere closes it as Esc does. A line or marker is gone
after 60 s of game time (1800 ticks), so a paused game keeps its marks; the in-game panel's row
*Board marks: Until erased* (`marklife=erased` in `impure.cfg`, `TS_MARKLIFE`) keeps them until
they are erased. The board reads the setting every frame, so a change applies to the marks already
made. The level's end clears everything.

**Who sees what.** Each player sees their own marks, their mutual allies' and, as a watcher,
everyone's; a watcher's marks are seen by watchers alone (`tagpu_share_board_seen`, the alliance
rule of the allied panel). An ally may edit, move, delete and erase any mark the player sees; any
other seat's operations change only that seat's own marks. In a network game every peer receives
every player's operations, whoever sees them, so every peer's recording holds them all; a peer
stores only those of the seats its player sees, so marks it does not draw never take a slot from
ones it does. A new
marker with text from a seat the player sees writes `Name: text` as a chat line in that seat's
colour, at most one line a seat each 30 ticks.

**Allies' pointers.** In a network game each ally's pointer is drawn where it is on the world: an
arrow in the ally's colour with a black edge, the ally's name below and right of it, and a dot in
the same colour on the minimap. A pointer on the minimap is drawn at the ground point it hovers;
one elsewhere on the HUD is not drawn. A watcher sees every player's.

**Finding marks.** Every marker and ally pointer is a dot on the minimap, and an ally's new marker
pings there. An ally's new marker off the view gets an arrow at the view's edge for 10 s; a click
on the arrow moves the camera to it, and `Ctrl`+`\` moves it to the last one received. Each ally's
chip on the allies' ribbon carries the ally's activity, a Mute and a Last marker
(*Finding marks*, below). The draw key's binding and marks in a replay are the later landings of
[the plan](../drawing-board-plan.html).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). The engine side is in the engine map: the key in [input](../engine/input.html), *`\` repeats the last typed command*; the world point and the router's stub in [input](../engine/input.html), *The router's fourth site*; the middle button in [UI](../engine/ui.html), *The engine has no wheel and no middle button of its own*.

## How a gesture is taken (`tagpu_board.c`, game thread)

**Two halves.** The window procedure (`tagpu_shield.c`, on the real path and on tacli's injected
one) decides whether a button is the board's: the key held, a level in play on the Vulkan lane
with a controlled seat that is not a puppet (`tagpu_route_world`), and the pointer on the world
view as the in-play handler last found it — `main+0x2CC6` bit 1 set, bit 3 (a selection box)
clear, no menu over it (`main+0x37EBE & 0x865`, `main+0x2BEE & 0xE0`). It takes that press whole,
and its release with it, so the engine sees neither: no selection box, no order. The middle
button is offered only to the board; the engine reads none.

Where the press lands in the world is not known there. A taken button is queued and resolved at
the next record the in-play handler `0x499200` passes the router's stub at `0x49939B` with
(`tagpu_route.c`, `right_button`, before the formation move). That record's world point
`main+0x2CAA` (16.16 x, altitude, z), written by `0x498DA0` under the zoom's transform, is the
press's: the engine hands its handler the mouse's last move every frame, and a press does not
move the pointer. A drag's moves reach the board the same way, so the board needs no point of its
own and is right at every zoom.

**Keys.** The key's `WM_KEYDOWN`, `WM_KEYUP` and `WM_CHAR` never reach the engine while the board
holds it, and neither does any character while the key is held or any key while the text box is
open; on the injected path the box's characters are made with `ToAscii` as the game's would be.
The key is not taken while a modal screen or the chat line holds the keyboard (`main+0x57D` not
-1). Its auto-repeat is taken and types nothing, open box or not; on the real path the key's own
`WM_CHAR`, which `TranslateMessage` has already posted when the key-down is dispatched, is removed
from the queue (posted messages are taken before input ones, so it is the one waiting).

**The hold is the key's state, not a message.** A release can be lost to something else — as
`WM_SYSKEYUP` while Alt is down, or to the in-game wiki, which takes every key-up while it is
open — so wherever the hold is used it is asked of the key: the shield's virtual state while the
shield is armed (`tagpu_shield_key_state`; the injected path sets it before the board is asked),
`GetAsyncKeyState` otherwise. A gesture lives inside the hold and ends with it, so a button
release the board never saw cannot leave a drag running. Focus loss ends the hold, the gesture and
the box (`tagpu_board_cancel`).

**Double clicks** are the board's own timing — two presses inside `GetDoubleClickTime` and 4
screen px — because the injected path delivers no `DBLCLK` and the engine's routing ignores one.

**Lines.** A drag adds a segment each time the pointer has moved 6 screen px from the last end,
however fast it moved, and one more from the last end to the release point when that is on the
world view (over the minimap the handler's point is the minimap's, across the map).

**The eraser** walks the whole path since the last record in steps of half its radius, so a fast
swipe crosses no line without erasing it. It tests the ground plane (x, z): a segment any part of
which lies within the radius goes, whole. A press on a pin erases nothing at the press point, so
a second press there can be the double click that deletes that marker alone.

**A marker's hit** is its pin: from its ground point to 30 screen px above, 9 either side; the
nearest the viewer wins when pins overlap.

## The operations and the store (`tagpu_boardwire.c`, `tagpu_boardstore.c`, pure C)

Every change to the marks is an **operation** — POINT (a new marker), EDIT (its text), MOVE,
DELETE, LINE (one segment), ERASE (a point the eraser passed) — applied by one state machine,
`tagpu_boardstore_apply(store, seat, op, tick, may)`. The player's own gestures apply theirs at
once; the channel's arrive in delivery order; a replay will apply a recording's. The same
operations at the same ticks under the same masks give the same marks.

The store keeps markers by id — the maker's seat in the top byte and the maker's sequence below,
never 0 — 256 of them, the oldest replaced when full; and line segments in a ring of 4096 that
drops the oldest. Each keeps its owner and its birth tick (`main+0x38A47`, GameTime). `may` is a
bit per seat whose marks the operation may change besides its own seat's. A POINT whose id exists
changes nothing. Nothing here is read by the simulation and nothing here writes engine memory;
`thread-split.allow` lists `tagpu_board.c` as `publisher presentation`, and the two pure files
name no engine memory.

**The sender's rights travel with the operation.** An EDIT, MOVE, DELETE or ERASE carries `may`
as its sender had it — the seats its player saw when it was made — and a peer applies it under
those rights cut to the seats its own player sees. A peer cannot work the sender's rights out for
itself: the engine keeps "j allies me" (`+0x113`) only for the controlled record
([share](share.html)). So an operation never changes more seats' marks on a peer than it could on
its sender — a player's erase over an enemy's line leaves it on a watcher's screen too.

**What is drawn is what is sent.** A gesture's operation is applied and queued only when the
outbox (16 KB) has room for it, and queued only when it changed something here. Past the outbox
the pen draws nothing, which happens only while the channel cannot drain (*Every frame*, below);
the level's line counts these as `outbox_full=`.

## In a network game (`tagpu_recorder.c`, the contribution channel)

The operations travel as the contribution kind **`RECMP_MARK` (16)**, whose body is
`tagpu_boardwire.h`'s: a version byte, the sender's seat, then whole operations. It runs where the
channel runs — every network game between Impure peers, recording or not
([recorder](recorder.html)) — and `RECMP_START_VERSION` 4 keeps an older peer from taking a game
whose first mark it would refuse.

**Every frame**, from the in-play handler (`tagpu_rec_mp_board_frame`, on the channel's own game
thread, after its first advance): the queued operations go out as bodies of at most
`RECMP_BODY_MAX` bytes, at the channel's capture tick, while the transmit queue keeps half its 256
slots free for the capture point's own kinds; then every `RECMP_MARK` at the head of the delivered
queue is taken and applied, up to the first packet of another kind. **That keeps delivery order**
— the capture point takes everything else as before, marks included when one is queued behind
another kind — and lets marks arrive while the game is paused, when no capture point runs, as long
as nothing of another kind is ahead of them: what the other peer's last capture point sent after
this peer's, or a click, waits for the next capture point, and the marks behind it with it. Their
application tick is the last capture point's, which no later take is below.

**At most two bodies a capture tick**, counted per tick rather than per frame. A pause holds the
tick, so however long a player draws in one, it adds at most two packets to each peer's receive
queue, which therefore cannot fill with marks (a full queue would end the channel's consumption,
the recording and the allies' resource sharing with it). Unpaused, two bodies of up to 4 KB a
tick carry far more than a player draws.

**The sender's numbering.** Every peer is its own record 0, so a seat number means nothing across
peers. A body names its seat and its markers' owners as its sender numbers them; the receiver
checks that the sender's own roster gives the packet's origin that seat, and renumbers the body's
seat and every marker id's owner through the channel's seat map (`tagpu_recmp_seat_map`,
`tagpu_boardwire_reseat`) before applying it. A marker id thus keeps its owner and sequence on
every peer. A body that fails any of this is dropped and counted in the level's `refused=`, never
refused on the channel: the board is display state, and a refusal would end every other kind's
consumption with it.

**Recordings.** The journal keeps the bodies as sent; format 29 is format 28 with them
(`tagpu_recfmt.h`). The history keeps any body of a source its roster names as an opaque record,
as the live game drops rather than refuses one, so a malformed body cannot end a recording. The
file tools read them (`tools/replay/recmp_file.py`, `board_ops`).

## The allies' pointers (`tagpu_rec_mp_cursors`, `tagpu_board_fill`)

No new traffic: the channel already carries every contributing human's camera and cursor five
times a second (`RECMP_POV`, `tagpu_recpov.h`) into the live cache the recorder keeps for its
recording (`tagpu_recmp_pov.h`, two samples an actor). `tagpu_rec_mp_cursors` reads it on the
channel's game thread at the channel's tick through the playback's own perspective clock
(`persp_sample`, `tagpu_persp.h`): one sample period (6 ticks, 200 ms) behind, so the two samples
around the shown moment are in hand, and interpolated between them when both are on the world
view; a pointer whose shown sample is on neither the world nor the minimap is left out. The board
puts each seat the player sees, other than the player's own, after the markers as a
`TAGPU_PK_BDMARK` with `TAGPU_PK_BDM_CURSOR`, its text the seat's name (`tagpu_share_name`),
followed by ` drawing...` or ` erasing...` while the ally's row on the ribbon shows it; the
table's cap is the store's 256 markers plus `TAGPU_PK_BD_CURSORS` (10), so a full store never
crowds a pointer out of the table. Only human seats on the channel are sampled, so an AI has no
pointer; a player never sees a watcher's, and whether another watcher does depends on the
watcher's having a roster actor (not measured). A pointer is the sender's data, which the channel
does not range-check: one past the packet's coordinate bound is left out.

The marker pass draws the replay overlay's arrow (`OV_ARROW`, its tip at the world point) filled in
the ally's colour over the same arrow in black a screen pixel either way, then the name plate,
after every marker; when more than about 130 long marker labels overflow the glyph bucket, the
pointers' name plates are the text that goes. The GUI's minimap compose (`tagpu_gui_surf.c`, after
the view box) maps the ground point into the box the base picture was drawn on, the inverse of the
engine's minimap arm (`(pointer - box) * extent / box`, `0x498DDA..0x498E19`,
[input](../engine/input.html)), whose y is projected: the point's x and `z - floor(h / 2)`, clamped
to the extent, then a 3-pixel dot on a 5-pixel black square.

## Finding marks (`tagpu_board.c`, `tagpu_gui_surf.c`, the allies' ribbon)

**The minimap.** The GUI's minimap compose draws every marker record of the packet over the view
box: a marker a 4-pixel square and an ally's pointer a 3-pixel one, in the owner's colour on a
black square 2 pixels larger, mapped as *The allies' pointers* says. A new marker from another seat
pings: a square outline in its colour whose side grows from 8 to 32 pixels over 2 s of wall time
(`PING_MS`, the record's `ping` counting down from `TAGPU_PK_BD_PING`). The quads are counted
against `TAGPU_GUI_SDRAW_BOARD` (640), the sharp layer's room past its own 32, which a static assert
holds above two quads a record and four a ping, so the board never costs the frame its cursor.

**New markers.** The board keeps the last 16 new markers that arrived from other seats the player
sees and has not muted (`s_recent`, wall time), each seat's last new marker (`s_lastOf`) and the
last of all (`s_lastAny`).

**Edge arrows.** A marker of that ring younger than 10 s (`RECENT_MS`), still on the board, whose
point lies off the view gets an arrow: a triangle in its owner's colour on a black one, pointing
at it, where the line from the view's centre to the point crosses the view's edge, 30 screen
pixels inside it. The view is the engine's: its centre the eye `main+0x1431F` plus half the 1x
view `main+0x37E37`, its half extents the 1x view over twice the zoom. The fill computes each
arrow on the game thread and keeps its anchor, so the arrow drawn and the arrow a click tests are
the same numbers. A left press on the world view within 16 screen pixels of an anchor, with the
draw key not held, is the board's: the engine never sees it, and the next frame moves the camera
so the marker is at the view's centre. The press keeps the arrow's marker, not its place in the
table, which every fill rebuilds. A jump goes through `tagpu_zoom_jump_eye`: the zoom's next
apply writes the eye and the scroll target, clamped, and releases the engine's unit follow, so
after a `Ctrl`+`C` the camera is not pulled back to the followed unit. An arrow whose anchor
would fall outside the packet's coordinate bound (an eye out of range) is left out.

**`Ctrl`+`\`** (the draw key with Ctrl down) starts no hold and moves the camera to the last new
marker received while it is still on the board and its owner is still seen and not muted, else to
the player's own last.

**The allies' ribbon** ([allied sharing](share.html), *The allies' ribbon*) gives each ally still
in the game a row under its cells (`tagpu_board_ally`, by the chip's player record index):

- `DRAWING...` or `ERASING...` in the ally's colour, for 700 ms (`ACT_MS`, wall time) after a line
  or an erase of that seat arrives;
- **Mute** (`amute:<index>`, pressed while on): the seat's lines, markers, pointer, pings, edge
  arrows and chat lines are not drawn on this screen, nor its activity. A muted seat's operations
  still arrive and still change its own marks, and change no one else's (the rights they apply
  with are none). The player's own gestures do not reach a muted seat's marks either: a press
  finds only marks of seats shown (seen and not muted), and the rights the player's operations
  carry are those seats, so an erase over a muted ally's hidden line leaves it on every peer.
  Unmuting shows the marks it kept. The level's end unmutes every seat, and Mute
  is not saved;
- **Last marker** (`alast:<index>`): moves the camera to that ally's last new marker while it is on
  the board and the ally is not muted, as `Ctrl`+`\` does. Without one it is drawn disabled, and
  its box is the control `ally_off`, which runs nothing, so the press does not open the give panel
  under it.

The ribbon draws only for a player in play with an ally, so a watcher, who sees every player's
marks, has no Mute and no Last marker.

## The packet

The packet carries the board by value (`tagpu_packet_pub.c`, `fill_board`): `TAGPU_PK_BOARD` (the
key, the pen, the text box and its text, this player's colour, the GUI black), at most 4096
`TAGPU_PK_BDSEG` and 256 `TAGPU_PK_BDMARK` of the seats the player sees, each segment and marker
with its owner's colour, read from the seat's logo (`tagpu_packet_pub_player_logo`,
`tagpu_replay_logo_colour`, cached per logo for the level). The fill drops what has outlived the
lifetime first. A cut sets
`TAGPU_PK_TRUNC_BOARD`. The acquire (`tagpu_packet.c`) bounds every coordinate to
±`TAGPU_PK_PLACE_COORD`, every text to its length with glyphs in range and a NUL, the flags to their
bits and the counts to the caps, so the marker pass reads them as given.

## How it is drawn (`tagpu_mark.c`, render thread)

In the marker pass, after the reclaim sweep and before the replay's overlays, with the fog off: a
mark an ally made is not something the viewer's sight hides. Four draws: the lines' black edges,
their colour (the sweep's outlined stroke, `stroke_into`), the pins' and plates' flat quads, the
text. A point is projected as the engine projects ground: x − eyeX + vpL, z − ⌊h/2⌋ − eyeY + vpT.
Pins, plates and text are a constant screen size, anchored on the device grid (`snap_device`). The
text box is the plate of the marker it edits, framed in the player's colour with a blinking caret;
the eraser shows a dashed ring of its radius at the pointer while the right button is held. A
segment both of whose ends lie more than 48 screen px past the same side of the view, or a marker
whose anchor lies that far outside it, makes nothing; a segment crossing the view with both ends
off it is drawn, cut by the line clip. A full bucket refuses the rest.

GDI draws none of it.

## How it was verified

`scenarios/build-facing.json` on Two Continents at 1024x768, the play arm set, input through
tacli's injected keys (`backslash` is the key's token), frames from the presented window,
MEASURED 2026-11-15:

- a drag drew a line on the ground through every corner of the path, to the release point;
  a middle click placed a pin; a double click opened a box that took typed characters, a space
  from the key path and a Backspace, and Return kept the text on the marker;
- a drag on the marker moved it; a right drag cut exactly the segment its single 170 px jump
  crossed; a right click on a pin left it standing and a right double click deleted it;
- the draw key held, the wiki opened by its emblem, the key released inside the wiki and the wiki
  closed: a plain right click then ordered the commander; two more key-downs of the draw key with
  the text box open typed nothing into it;
- a single segment drawn at zoom 0.5 stayed drawn at zoom 4 with both its ends off the view;
- the selected commander stayed at [1600, 1600] through every gesture (the input log: every
  button the board's, none the engine's); after the key was released inside an open box and the
  box committed, a plain right click ordered the commander and it walked;
- a line drawn while paused drew; zoom 2 and 0.5 kept lines on the ground and pins at their size;
- everything made more than 60 s before a frame was gone from it; the level's end logged
  `board: level end marks=0 segs=1 erases=0 dropped=0`.

Two-player network games on Two Continents, both cameras pinned to one eye, MEASURED 2026-11-16:

- **allied** (`mp_lobby.sh --allies`): the host's line and its marker `Hi` drew on the joiner,
  with the chat line `BD-H: Hi` led by the host's colour, and the joiner's pin on the host;
- **paused** (the tick held at 626 on both): the joiner's line drew on the host, and the host's
  drag on the joiner's pin moved it on both screens; after the pause both channels read
  `failed=00000000` with nothing refused;
- **not allied**: each peer logged the other's bodies (`recmp: remote marks …`) and drew none of
  them, and no chat line came;
- both peers recorded (format 29, 0 corrections); `tarec.py check` read each to its end, the
  contributions through `recmp_file.History`, and the host's file played in the game past its
  marks' ticks with nothing refused;
- **the sender's rights** (allied): the joiner's right drag over the host's line erased it on both
  screens;
- **a flood in a pause**: 300 pen steps drawn on the joiner in a pause reached the host as two
  bodies until the pause ended and the rest within 4 s of it; 900 more in a second pause drew until
  the outbox was full and no further on the joiner, and after the pause the host showed exactly the
  same rows. The level's lines: the joiner `segs=956 erases=2 outbox_full=243`, the host
  `received=958 refused=0`; both channels `failed=00000000`, both recordings read to their end.

Allies' pointers, MEASURED 2026-11-16 in the same games: allied, the joiner's pointer at (600, 350)
drew on the host at (600, 350) as a red arrow named `BD-J`, and as a red dot inside the host's view
box on the minimap; on the side panel it was not drawn, and back on the world at (300, 250) it
was; not allied, the host drew none.

Finding marks, MEASURED 2026-11-16 in allied games: the joiner's marker off the host's view drew
an edge arrow in its colour and pinged on the host's minimap; a click on the arrow, and in another
round `Ctrl`+`\`, put the marker at the host's view centre; while the joiner drew, its pointer's
plate read `BD-J drawing...`. On the ribbon (HUD 100 % at 1024x768): during the joiner's stroke the
host's chip read `DRAWING...` in red, gone 2 s later; Mute (`tacli hud bd-h click amute:1`) took
the joiner's marker and pointer off the host's minimap, leaving the engine's own dot for its
commander, disabled Last marker, and a second press brought them back; once the marker had faded
Last marker was disabled; after a new marker, Last marker put it at the view centre; a press on the
disabled Last marker opened no give panel, and one on the chip still did.

The lifetime setting, MEASURED 2026-11-16 in a skirmish (`--defaults`): the panel's row stored
`marklife=erased`; a line outlived 2440 ticks (81 s of game time); switching back to *Fade 60 s*
removed it at once and stored `marklife=60`.

Offline: `tools/replay/board_test.py` builds `board_test.c` with the sanitizers — every operation
there and back, each truncation and malformed field refused, the renumbering, the store's rights,
eraser radius, ring, lifetime, a tick that goes back and the oldest marker's replacement — and
reads the body it writes with `recmp_file.board_ops`.

## Gaps

- No marks in a replay yet, and the draw key is not rebindable — landings 5 and 6 of the plan.
- The lifetime is each player's own setting and is not sent, so allies with different settings
  see different boards, and an edit or move of a marker one peer has already dropped does nothing
  there.
- A watcher has no Mute and no Last marker: the allies' ribbon, which carries them, is a player's.
- An ally out of the game has no board row on its chip, so it can no longer be muted or unmuted
  from there; its marks fade or stay as the lifetime says.
- Allies' pointers freeze while the game is paused: their samples are taken at capture points.
- The pointer's motion between samples was not measured frame by frame; the interpolation is the
  playback's, which `tools/replay/persp_test.py` checks.
- Two peers editing one marker at once can end with different text on each: each applies its own
  operation at once and the other's when it arrives.
- A watcher's own marks are sent only when the channel's roster has an actor for the watcher's
  seat; not measured.
- The spam limit is the chat line's (one a seat each 30 ticks); the marks themselves are bounded by
  the store, the outbox and two bodies a tick, not by a rate.
- In a pause, marks queued behind another kind wait for play to resume, and a player who fills the
  outbox meanwhile draws nothing more until then.
- An alliance made mid-game does not bring back the new ally's earlier marks: a peer stores only
  the seats its player saw when each operation arrived.
- The key is fixed at `VK_OEM_5`, which types `\` on a US layout and another key elsewhere.
- No real-mouse run yet: the measurements are tacli's injected input on the shield's path. The
  real window-procedure path shares the module's calls but was not driven; the removal of the draw
  key's `WM_CHAR` and the hardware key poll are on that path only.
