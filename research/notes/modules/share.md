# Sharing resources with allies

In a network game the player sets how much metal and energy they keep by dragging a tick on the
game's own resource bar; above that level the engine's auto-share gives the excess to an ally. The
plan, the decisions behind it and the landings still to come are in
[the shared resources plan](../shared-resources-plan.html).

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

## The share tick

**When it is there.** In a network game (`main+0x2A44` bit 0, the share commands' own test), while
the top bar shows the controlled player (`main+0x2A42` = `main+0x2A43`), whose record is active,
has its PlayerInfo and is not a watcher (PlayerInfo `+0x9B & 0x40`), on a side the five side slots
hold. Each bar needs its rect inside the top bar's 32 rows with some length, and a storage above 0
(the engine's own fill skips a storage of 0). Otherwise nothing is drawn and no press is taken.

**Where.** On each resource bar's own rect, the viewed side's `RaceSideData` `+0x72` (metal) and
`+0x52` (energy) at `main + 0x37F3D + 0x232·s`, the rects the engine fills: so a mod's bar layout
carries the tick with it. Its left column is the engine's own tick formula,
`ftol(x1 + (x2 − x1) · level / storage)`, held inside the bar; at the right end while sharing is
off.

**What it looks like.** Three columns of fill over the bar's rows and two rows above and below,
with a column of black (palette 0) on each side. The fill is the engine's tick colour, GUI colour
`0xC`, while sharing and holding more than the level; palette 24 (111, 51, 59) while sharing and
holding no more; palette 89 (107, 107, 107) while not sharing. While the pointer is on the top bar,
or during a drag, a line under each bar says *shares metal above 2400*, *shares all metal* or
*shares no metal*, in the side's font and GUI colour `0xC`.

**How it is drawn.** From the post-GUI observer at `0x46A308`, after DrawGameScreen's GUI draw and
before the flip, with the engine's own `DrawBar 0x4BF6F0(NULL, rect, colour)` and
`DrawTextCustomFont 0x4C14F0(NULL, text, x, y, −1)`, the font and colours saved and put back
around the text. Those are the calls the UI's op ring captures, so the tick reaches every renderer
as the bar itself does. It covers the engine's own tick exactly when that one is drawn, at the same
column. Under HUD scale the bar is magnified with the rest of the top bar; the line under it sits in
the world region, which is moved rather than magnified, so it is placed where the magnified bar's
left end lands and moved back along that vector, as the Config panel is.

**Input.** A left press on a bar's strip (the bar, 4 columns past each end and 9 rows above and
below, inside the top bar) is the share tick's: it sets the level the pointer's column names and
starts a drag; each draw during the drag sets it again from the pointer; the release ends it. The
press and its release never reach the engine. The hit-test sits on both input paths through
`tagpu_menu_click` and `tagpu_menu_owns_point` (the injected path a tacli instance uses, and the
real one), and in engine coordinates, which is the space both paths hand it.

**What a press writes.** The level is the column's share of the storage, snapped to 50. From the
right end, or a level that reaches the storage, sharing is off: the bit is cleared and the level
set to 0, so the engine's own tick, drawn only for a level above 0, never shows beside this one.
Otherwise the bit is set and the level written. The level is written as SetShareMetal and
SetShareEnergy write it and, like theirs, is never sent to another peer; the bit is toggled only
when it changes, followed by the engine's PlayerInfo broadcast `0x450F90`, as the ShareMetal and
ShareEnergy commands do. The commands themselves are not called: each prints a chat line.

**What it does not do.** It changes no rule of the auto-share: one ally a cycle, every 60 ticks,
the highest-numbered one holding less than the player. It writes nothing outside a network game,
for another record, or for a watcher.

## Gaps

- A drag's release that never arrives while the window keeps the focus ends the drag only at the
  next press.
- The palette entries 24 and 89 are the stock palette's; a mod with its own palette shows its own
  colours there.
