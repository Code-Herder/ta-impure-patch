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
after 60 s of game time (1800 ticks), so a paused game keeps its marks. The level's end clears
everything.

**Who sees what.** Each player sees their own marks, their mutual allies' and, as a watcher,
everyone's; a watcher's marks are seen by watchers alone (`tagpu_share_board_seen`, the alliance
rule of the allied panel). An ally may edit, move, delete and erase any mark the player sees; any
other seat's operations change only that seat's own marks. In a network game every peer receives
every player's operations, whoever sees them, so every peer's recording holds them all. A new
marker with text from a seat the player sees writes `Name: text` as a chat line in that seat's
colour, at most one line a seat each 30 ticks.

Ally cursors, the minimap, the edge arrows and the settings are the later landings of
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

A gesture's operation is queued for the channel only when it changed something here: a peer
applies it under its own masks, which never let it change more than it changed here.

## In a network game (`tagpu_recorder.c`, the contribution channel)

The operations travel as the contribution kind **`RECMP_MARK` (16)**, whose body is
`tagpu_boardwire.h`'s: a version byte, the sender's seat, then whole operations. It runs where the
channel runs — every network game between Impure peers, recording or not
([recorder](recorder.html)) — and `RECMP_START_VERSION` 4 keeps an older peer from taking a game
whose first mark it would refuse.

**Every frame**, from the in-play handler (`tagpu_rec_mp_board_frame`, on the channel's own game
thread, after its first advance): the queued operations go out as bodies of at most
`RECMP_BODY_MAX` bytes, four a frame at most and only while the transmit queue keeps 64 slots
free, at the channel's capture tick; then every `RECMP_MARK` at the head of the delivered queue is
taken and applied, up to the first packet of another kind. **That keeps delivery order** — the
capture point takes everything else as before, marks included when one is queued behind another
kind — and lets marks arrive while the game is paused, when no capture point runs. Their
application tick is the last capture point's, which no later take is below.

**The sender's numbering.** Every peer is its own record 0, so a seat number means nothing across
peers. A body names its seat and its markers' owners as its sender numbers them; the receiver
checks that the sender's own roster gives the packet's origin that seat, and renumbers the body's
seat and every marker id's owner through the channel's seat map (`tagpu_recmp_seat_map`,
`tagpu_boardwire_reseat`) before applying it. A marker id thus keeps its owner and sequence on
every peer. A body that fails any of this is dropped and counted, never refused: the board is
display state, and a refusal would end every other kind's consumption with it.

**Recordings.** The journal keeps the bodies as sent; format 29 is format 28 with them
(`tagpu_recfmt.h`), and the history accepts a well-formed body of a source its roster names. The
file tools read them (`tools/replay/recmp_file.py`, `board_ops`).

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
  marks' ticks with nothing refused.

Offline: `tools/replay/board_test.py` builds `board_test.c` with the sanitizers — every operation
there and back, each truncation and malformed field refused, the renumbering, the store's rights,
eraser radius, ring, lifetime, a tick that goes back and the oldest marker's replacement — and
reads the body it writes with `recmp_file.board_ops`.

## Gaps

- No ally cursors, minimap marks or pings, edge arrows, per-ally mute, settings, or marks in a
  replay yet — landings 3 to 6 of the plan.
- Two peers editing one marker at once can end with different text on each: each applies its own
  operation at once and the other's when it arrives.
- A watcher's own marks are sent only when the channel's roster has an actor for the watcher's
  seat; not measured.
- The spam limit is the chat line's (one a seat each 30 ticks); the marks themselves are bounded by
  the store, the body and the channel's queue, not by a rate.
- The key is fixed at `VK_OEM_5`, which types `\` on a US layout and another key elsewhere.
- No real-mouse run yet: the measurements are tacli's injected input on the shield's path. The
  real window-procedure path shares the module's calls but was not driven; the removal of the draw
  key's `WM_CHAR` and the hardware key poll are on that path only.
