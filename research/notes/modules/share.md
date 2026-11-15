# Sharing resources with allies

In a network game the player sets how much metal and energy they keep by dragging a tick on the
game's own resource bar; above that level the engine's auto-share gives the excess to an ally. Under
the top bar a ribbon of chips shows each ally's metal and energy. The plan, the decisions behind it
and the landings still to come are in [the shared resources plan](../shared-resources-plan.html).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the plan.*

Addresses are VAs for our pristine build. `main` = `*(void**)0x511DE8`; player record `p` =
`main + 0x1B63 + p*0x14B`. The engine's side of sharing (the auto-share, the share commands, the
alliance rows, the transfer and its messages) is in the engine map: [recorder](../engine/recorder.html)
(the auto-share and share commands' rows), [network](../engine/network.html) (`0x450F90`,
`0x452960`, `0x457050`, the `0x28`'s cadence) and [units](../engine/units.html) (`+0xE4`/`+0xE8`,
`+0x108`).

## The files

| file | thread | what |
|---|---|---|
| `tagpu_sharetick.c/.h` | game | the share tick: drawn, hit-tested, dragged; writes the level and the bit |
| `tagpu_menu.c` | game | its post-GUI observer at `0x46A308` calls `tagpu_sharetick_draw`; its click and owns-point entries ask the share tick first |
| `tagpu_shield.c` | game | a lost mouse (`WM_KILLFOCUS`, `WM_CAPTURECHANGED`, deactivation) ends a drag |
| `tagpu_sharetick.c` (`tagpu_share_allies`) | game | the mutual allies, read from their records for the ribbon |
| `tagpu_hudstate.c` | game, render | the HUD's `allies` mode: the allies into the packet's HUD record (`fill_allies`), then the record into the layout's state (`tagpu_hudstate_present`) |
| `tagpu_hudlay.c` (`allies`), `tools/hudart/mockup/hud.js` (`allies`) | render | the ribbon's layout, the C a port of the mockup's, held to it by `tools/hudart/laycheck.py` |
| `tagpu_hudinput.c` | game | a press on a chip is the HUD's, and runs nothing |
| `art/hud/layout.json` `proposed.allies`, `art/hud/parts/lamp_on.png`, `lamp_off.png` | — | the ribbon's numbers and the lamp's two parts (`tools/hudart/lamp.py` makes them from their paintings) |

## The share tick

**When it is there.** In a network game (`main+0x2A44` bit 0, the share commands' own test), in a
level the engine has drawn at least once and is not tearing down (`tagpu_packet_pub_level_open`,
`tagpu_reclaim_level_closing`; the network bit is already set in the battle room, and during the
load the loader thread writes the same record and PlayerInfo), while the top bar shows the
controlled player (`main+0x2A42` = `main+0x2A43`), whose record is active,
has its PlayerInfo and is not a watcher (PlayerInfo `+0x9B & 0x40`), on a side the five side slots
hold. Each bar needs its rect inside the top bar's 32 rows with some length, and a storage above 0
(the engine's own fill skips a storage of 0). Otherwise nothing is drawn and no press is taken.

**Where.** On each resource bar's own rect, the viewed side's `RaceSideData` `+0x72` (metal) and
`+0x52` (energy) at `main + 0x37F3D + 0x232·s`, the rects the engine fills: so a mod's bar layout
carries the tick with it. Its left column is the engine's own tick formula,
`ftol(x1 + (x2 − x1) · level / storage)`, held inside the bar; at the right end while sharing is
off with a level of 0. The engine draws its own tick whenever the level is not 0 and the stored
value is above it, whatever the bit, so a level the console set while sharing is off is drawn grey
at its column, over the engine's.

**What it looks like.** Three columns of fill over the bar's rows and two rows above and below,
with a column of black (palette 0) on each side. The fill is the engine's tick colour, GUI colour
`0xC`, while sharing and holding more than the level; palette 24 (111, 51, 59) while sharing and
holding no more; palette 89 (107, 107, 107) while not sharing. While the pointer is on the top bar,
or during a drag, a line under each bar says *shares metal above 2400*, *shares all metal* or
*shares no metal*, in the side's font and GUI colour `0xC`.

**How it is drawn.** From the post-GUI observer at `0x46A308`, after DrawGameScreen's GUI draw and
before the flip, with the engine's own `DrawBar 0x4BF6F0(NULL, rect, colour)` and
`DrawTextCustomFont 0x4C14F0(NULL, text, x, y, −1)`, the font and colours saved and put back
around the text. Those are the calls the UI's op ring captures, so the tick reaches the screen as
the bar itself does (seen on Vulkan; new features take no GDI tests). It covers the engine's own
tick exactly when that one is drawn, at the same column.

**A tick moves only over a freshly painted bar.** DrawGameScreen repaints its resource block only
when its 33-byte memo at `main+0x37E3F` differs from the live values ([render](../engine/render.html),
*The resource block, and the 33-byte memo that skips it*), so a full, steady bar keeps every pixel
drawn on it, and a tick drawn at a new column would leave the old one beside it. When a tick's column
changes, it stays drawn where it is for one frame and the memo's byte 0 is set to the viewed record's
`+0x146` ^ `0xFF`, a value the block never computes and never reads back (the same lever
`tagpu_gui_hook.c` uses to repaint the block after a reset). DrawGameScreen has no exit or jump
between its entry and the block, and the tick draws later in the same call, so the next draw follows
a compare that saw the spoiled byte and repainted; the tick moves on the frame the byte no longer
reads as spoiled. A level changed by any path, the console commands included, moves the same way. Under HUD scale the bar is magnified with the rest of the top bar; the line under it sits in
the world region, which is moved rather than magnified, so it is placed where the magnified bar's
left end lands and moved back along that vector, as the Config panel is.

**Input.** A left press on a bar's strip (the bar, 4 columns past each end and 9 rows above and
below, inside the top bar) is the share tick's: it sets the level the pointer's column names and
starts a drag; each draw during the drag sets it again from the pointer; the release ends it. A
double click on the strip is a press like any other (the window class has `CS_DBLCLKS`, `0x4B5B89`).
The press and its release never reach the engine. On the real path a press the tick takes holds
the mouse (`SetCapture`) until its release, so the release arrives wherever the button goes up, or
`WM_CAPTURECHANGED` ends the drag when another window takes the mouse; the in-game wiki, which takes
every input message while open, ends a drag too. The hit-test sits on both input paths through
`tagpu_menu_click` and `tagpu_menu_owns_point` (the injected path a tacli instance uses, and the
real one), and in engine coordinates, which is the space both paths hand it.

**What a press writes.** The level is the column's share of the storage, snapped to 50. From the
right end, or a level that reaches the storage, sharing is off: the bit is cleared and the level
set to 0, so the engine's own tick, drawn only for a level above 0, never shows beside this one.
Otherwise the bit is set and the level written. The level is written as SetShareMetal and
SetShareEnergy write it and, like theirs, is never sent to another peer; the bit is toggled only
when it changes, followed by the engine's PlayerInfo broadcast `0x450F90`, as the ShareMetal and
ShareEnergy commands do. The commands themselves are not called: each prints a chat line.

**Every level starts with sharing off.** At the first in-play draw of each level (a new
`tagpu_reclaim_level_gen`) the controlled record's bits 2 and 4 are cleared and, when one was set,
the PlayerInfo is broadcast, whatever path led to the level. No field write of the engine clears
those bits: every write to `+0x97` outside the console commands touches bit 0 alone (`0x4280F3`,
`0x451334`, `0x45193A`, the reset's `0x46460F`). Yet after a surrender, the main menu and a new
battle room the second game started with them clear before this ran (MEASURED 2026-11-14, the
metal bit set in the first game), so something else, a copy or a re-creation of the block, clears
them on that path; which one is not mapped.

**What it does not do.** It changes no rule of the auto-share: one ally a cycle, every 60 ticks,
the highest-numbered one holding less than the player. It writes nothing outside a network game,
for another record, or for a watcher.

## The allies' ribbon

**When it is there.** In a network game, for the player in play as the share tick finds it (the
same lifetime and the same refusals: not for a watcher, and not in a skirmish or a replay, which
plays a network recording as a single-player game), while that player has at least one ally, and
only on the Vulkan lane, which draws the HUD. It is the HUD's third mode beside the replay's and the
live rewind's (`tagpu_hudstate_mode`, [the HUD in game](../hud-in-game.html), *When it draws*); those
two never show in a network game, so the ribbon replaces neither.

**Who gets a chip.** Every other player record that is active, remote (`+0x73` = 3), has its
PlayerInfo, is not a watcher (PlayerInfo `+0x9B & 0x40`), and is allied both ways: the controlled
record's row `+0x108[j]` is 1 and the other record's row toward the controlled player is 1. The rows
are one-way, and a peer hears of another player's row toward it from that player's `0x23`, so these
are the two rows this peer is told of. Chips run in record order.

**What a chip shows.** The replay HUD's kit: the `lanes_plate` plate, the player's colour as a
stripe down its left edge, the name in the `header` style, and a `pick_row` cell for metal over one
for energy, each with the stored value over the storage as a strip along its bottom in the resource's
colour (`#d3dbff`, `#ffff00`). The colour is the middle pixel of the player's logo frame (PlayerInfo
`+0x96`) in the engine's colour sequence, as the replay HUD colours its seats
(`tagpu_replay_logo_colour`). The stored values and storages are the remote record's, which the
owner's `0x28` refreshes every 120 ticks; a value that is not finite shows as 0.

**The lamp.** A chip whose metal or energy is under 2 % of its storage carries a square lamp at its
top right that blinks: lit for the first half of every 0.9 s, dark for the second, by the render
thread's wall clock, so it blinks on while the game is paused. Without the allies' rates this is the
2 % test alone. The lit part carries its red glow round the painting.

**Where.** Right-aligned under the top bar, the first row 4 units below its lower edge and 6.667 from
the screen's right; chips 85.333 × 45.333 units with 3.333 between chips and between rows. A row
holds as many chips as fit between the side panel (plus 4 units) and the screen's right; the rest
wrap to rows below, each right-aligned. A HUD unit is the replay HUD's (the viewport height / 720 ×
its scale, the `replayhud` setting), so the HUD scale applies, and the top bar's height is the
engine's at the game's HUD scale.

**Input.** Each chip is a control of the HUD's hit map (key `ally:<record index>`): a press on it is
the HUD's, not the world's, and does nothing yet. The HUD takes no press while a menu covers the
world, as for its other modes.

## Gaps

- A negative level, which only the console sets (SetShareMetal has no lower bound), puts the
  engine's tick left of the bar while this one is held at its left end.
- A replay plays a network recording as a single-player game, so it shows neither this tick nor
  the share line; it shows the engine's tick wherever the recorded level puts it.
- The palette entries 24 and 89 are the stock palette's; a mod with its own palette shows its own
  colours there.
- The ribbon shows the allies' stored values and storages only: no incomes, no spending and no
  share levels, and so the lamp's 2 % test without the rates.
- A player who has left the game, or whose units are all gone, keeps a chip while its record stays
  active and allied; how the engine marks a departed player's record is not mapped.
- An ally hosted on this peer (a computer player, `+0x73` 1 or 2) gets no chip.
- On GDI there is no ribbon.
