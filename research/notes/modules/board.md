# Drawing board: lines and markers on the map

Hold **`\`** over the world view and draw on the ground:

| gesture (with `\` held) | does |
|---|---|
| left drag | a line on the ground, in the player's colour on a black edge |
| middle click | a marker: a pin in the player's colour |
| left double click | a text box: on a marker, its text; elsewhere, a new marker with text |
| left drag on a marker | moves it |
| right drag | erases every line segment and marker within 48 world px of the path |
| right double click on a marker | deletes it |

In the text box, the engine font's glyphs `0x20..0x7E` type, up to 60; Backspace deletes, Return
keeps the text and Esc drops it; a click anywhere closes it as Esc does. A line or marker is gone
60 s after it was made. The level's end clears everything.

This is the local half: what one player draws is drawn on their own screen only. The channel to
allies, their cursors, the minimap and the settings are the later landings of
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
-1). The key's release always ends the hold, so a box opened while the key was down cannot leave
it held. Focus loss ends the hold, the gesture and the box (`tagpu_board_cancel`).

**Double clicks** are the board's own timing — two presses inside `GetDoubleClickTime` and 4
screen px — because the injected path delivers no `DBLCLK` and the engine's routing ignores one.

**Lines.** A drag adds a segment each time the pointer has moved 6 screen px from the last end,
however fast it moved, and one more from the last end to the release point.

**The eraser** walks the whole path since the last record in steps of half its radius, so a fast
swipe crosses no line without erasing it. It tests the ground plane (x, z): a segment any part of
which lies within the radius goes, whole.

**A marker's hit** is its pin: from its ground point to 30 screen px above, 9 either side; the
nearest the viewer wins when pins overlap.

## The store and the packet

Markers by id (the owner's seat in the top byte, a per-seat sequence below), 256 of them, the
oldest replaced when full; line segments in a ring of 4096 that drops the oldest. Each keeps its
owner and its birth time (`GetTickCount`). Nothing here is read by the simulation and nothing here
writes engine memory; `thread-split.allow` lists the file as `publisher presentation`.

The packet carries the board by value (`tagpu_packet_pub.c`, `fill_board`): `TAGPU_PK_BOARD` (the
key, the pen, the text box and its text, this player's colour, the GUI black), at most 4096
`TAGPU_PK_BDSEG` and 256 `TAGPU_PK_BDMARK`, each segment and marker with its owner's colour, read
from the seat's logo (`tagpu_packet_pub_player_logo`, `tagpu_replay_logo_colour`, cached per logo
for the level). The fill drops what has outlived the 60 s first. A cut sets
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
segment both of whose ends, or a marker whose anchor, lies 48 screen px outside the view makes
nothing; a full bucket refuses the rest.

GDI draws none of it.

## How it was verified

`scenarios/build-facing.json` on Two Continents at 1024x768, the play arm set, input through
tacli's injected keys (`backslash` is the key's token), frames from the presented window,
MEASURED 2026-11-15:

- a drag drew a line on the ground through every corner of the path, to the release point;
  a middle click placed a pin; a double click opened a box that took typed characters, a space
  from the key path and a Backspace, and Return kept the text on the marker;
- a drag on the marker moved it; a right drag cut exactly the segment its single 170 px jump
  crossed; a right double click deleted a marker;
- the selected commander stayed at [1600, 1600] through every gesture (the input log: every
  button the board's, none the engine's); after the key was released inside an open box and the
  box committed, a plain right click ordered the commander and it walked;
- a line drawn while paused drew; zoom 2 and 0.5 kept lines on the ground and pins at their size;
- everything made more than 60 s before a frame was gone from it; the level's end logged
  `board: level end marks=0 segs=1 erases=0 dropped=0`.

## Gaps

- Local only: allies see nothing yet, and there are no ally cursors, minimap marks, edge arrows or
  settings — landings 2 to 6 of the plan.
- The lifetime is wall time, so marks fade while the game is paused.
- The key is fixed at `VK_OEM_5`, which types `\` on a US layout and another key elsewhere.
- No real-mouse run yet: the measurements are tacli's injected input on the shield's path. The
  real window-procedure path shares the module's calls but was not driven.
