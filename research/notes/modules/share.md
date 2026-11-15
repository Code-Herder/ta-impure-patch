# Sharing resources with allies

In a skirmish or a network game the player sets how much metal and energy they keep by dragging a
tick on the game's own resource bar; above that level the engine's auto-share gives the excess to an
ally. Under the top bar a ribbon of chips shows each ally's metal and energy, and a press on a chip
opens a panel that gives that ally metal and energy. An AI ally is an ally like a human one, whichever
peer hosts it. The plans and the decisions behind them are [the shared resources
plan](../shared-resources-plan.html) and [the AI-allies plan](../shared-resources-ai-plan.html).

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
| `tagpu_sharetick.c` (`tagpu_share_allies`, `tagpu_share_stored`, `tagpu_share_give`) | game | the mutual allies, read from their records for the ribbon, with their economy from the contribution channel while it runs; the player's own stored values; the give, run from the post-GUI observer |
| `tagpu_recorder.c` (`tagpu_rec_mp_economy`) | game | an Impure player's economy out of the contribution channel's player cache, while the channel runs and this peer applies it |
| `tagpu_hudstate.c` | game, render | the HUD's `allies` mode: the allies into the packet's HUD record (`fill_allies`), then the record into the layout's state (`tagpu_hudstate_present`) |
| `tagpu_hudlay.c` (`allies`), `tools/hudart/mockup/hud.js` (`allies`) | render | the ribbon's layout, the C a port of the mockup's, held to it by `tools/hudart/laycheck.py` |
| `tagpu_hudinput.c` | game | the give panel's state: a chip opens and closes it, its sliders set the amounts, Send asks the give |
| `art/hud/layout.json` `proposed.allies`, `art/hud/parts/lamp_on.png`, `lamp_off.png` | — | the ribbon's numbers and the lamp's two parts (`tools/hudart/lamp.py` makes them from their paintings) |

## The share tick

**When it is there.** In a skirmish or a network game, in a level the engine has drawn at least
once and is not tearing down (`tagpu_packet_pub_level_open`, `tagpu_reclaim_level_closing`; during
the load the loader thread writes the same record and PlayerInfo), while the top bar shows the
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
drawn on it, and a tick drawn at a new column would leave the old one beside it. When a tick's
column changes, it stays drawn where it is and a repaint is asked through the top bar's repaint
coordinator, which the weather report shares ([top bar](topbar.html), *The repaint*); the tick moves
in the frame whose block has repainted. A level changed by any path, the console commands included, moves the same way. Under HUD scale the bar is magnified with the rest of the top bar; the line under it sits in
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
the highest-numbered one the player allies holding less than the player, a third of the metal and
half the energy above the level. It writes nothing for another record or for a watcher. Who the
auto-share reaches, and where it runs, is *AI allies* below.

## The allies' ribbon

**When it is there.** In a skirmish or a network game, for the player in play as the share tick
finds it (the same lifetime and the same refusals: not for a watcher, and not in a replay, whose
viewer sits in record 10), while that player has at least one ally, and only on the Vulkan lane,
which draws the HUD. It is the HUD's third mode beside the replay's and the live rewind's
(`tagpu_hudstate_mode`, [the HUD in game](../hud-in-game.html), *When it draws*); a skirmish with an
ally shows the ribbon, which the live rewind's HUD replaces while it is open.

**Who gets a chip.** Every other player record that is active, a local AI (`+0x73` = 2) or remote
(3: a human, or an AI another peer hosts), has its PlayerInfo, is not a watcher (PlayerInfo `+0x9B &
0x40`), and is allied both ways: the controlled record's row `+0x108[j]` is 1, and j allies the
controlled player — this peer's mirror `+0x113[j]`, the engine's own "j allies me" (its victory test
`0x490080` reads the row and the mirror), or j's own row toward the controlled player. A network game
keeps the mirror: a remote player's `0x23` sets it, and allying an AI sets it at once, the engine
taking an AI to reciprocate; a skirmish's seat fill writes the rows alone, both ways for a shared
ally group. Chips run in record order.

**What a chip shows.** The replay HUD's kit: the `lanes_plate` plate, the player's colour as a
stripe down its left edge, the name in the `header` style, and a `pick_row` cell for metal over one
for energy, each with the stored value over the storage as a strip along its bottom in the resource's
colour (`#d3dbff`, `#ffff00`). The colour is the middle pixel of the player's logo frame (PlayerInfo
`+0x96`) in the engine's colour sequence, as the replay HUD colours its seats
(`tagpu_replay_logo_colour`).

**The rates and share marks.** A local AI's come from its own record, which the economy pass keeps
on this peer: exact, with or without the channel. A remote ally's come from the recorder's
contribution channel while it runs (every seat runs Impure). Either way each cell also shows the ally's income and spending, `+income` at its left in the signs'
green and `−spending` at its right in their red (`num_sm`, tenths below 100), above the strip; and
while the ally shares the resource (PlayerInfo `+0x97` bit 2 or 4), a red mark edged in black at its
share level across the strip. The numbers are the ally's own: its player image as the channel last
delivered it (`tagpu_rec_mp_economy`, by the record's DirectPlay ID `+0x04`), from which the chip
also takes the stored values and storages (record `+0x98`, `+0x8C`, `+0xA8`, `+0xA4`, income
`+0x9C`, `+0x90`, spending `+0xA0`, `+0x94`, levels `+0xE4`, `+0xE8`). They are data from another
peer, shown and never fed to the simulation: an image is taken only when every stored value,
storage and rate in it is finite and not negative, and a level is drawn only inside 0..that image's
storage.

**Without the channel** — a channel that failed on this peer (the
recorder ends its cache), this peer having stopped applying it (the recorder's consuming latch),
or that ally's own source no longer contributing (its final seal, or its private player gone from
the channel's session, `tagpu_recmp_source_live`) — a remote ally's chip shows the storage strips alone, from
the remote record, which the owner's `0x28` refreshes every 120 ticks — a remote AI's too, its host
sending it beside its human's (*AI allies*); a value that is not finite shows as 0.

**The lamp.** Every chip carries a square lamp at its top right, dark. While the ally's metal or
energy is under 2 % of its storage it blinks: lit for the first half of every 0.9 s, dark for the second, by the render
thread's wall clock, so it blinks on while the game is paused. With the allies' rates the lamp also
needs that resource's spending above its income; without them it is the 2 % test alone. The lit part carries its red glow round the painting.

**Where.** Right-aligned under the top bar, the first row 4 units below its lower edge and 6.667 from
the screen's right; chips 85.333 × 45.333 units with 3.333 between chips and between rows. A row
holds as many chips as fit between the side panel (plus 4 units) and the screen's right; the rest
wrap to rows below, each right-aligned. A HUD unit is the replay HUD's (the viewport height / 720 ×
its scale, the `replayhud` setting), so the HUD scale applies, and the top bar's height is the
engine's at the game's HUD scale.

**Input.** Each chip is a control of the HUD's hit map (key `ally:<record index>`, the index on
this peer, which numbers itself 0; `ally_off` for an ally out of the game): a press on it is the
HUD's, not the world's, and opens the give panel on that ally, or closes it when it is open there. As for the HUD's other modes, the hit test
is in client pixels on both input paths, and the HUD takes no press while a menu covers the world.

**An ally out of the game.** A chip whose ally is out by the transfer's own send test (no unit alive,
record `+0x144`, a word, after at least one was created, `+0x140`; or an own index `+0x146` of 10)
is drawn at 45 % and its press runs nothing; a give panel open on it closes. An ally that has not
created its first unit yet is in the game.

## The give panel

**What it shows.** Under the chip, `picker_one`'s plate 220 units wide, held between the side panel
and the screen's right: *Give to* and the name, a line a resource with the ally's stored value and
storage, a slider a resource with the amount asked beside it in the resource's colour, and Send
(`chip_face`), drawn disabled while both amounts are 0. A slider is the live strip's groove
(`live_groove`) filled to the amount in the resource's colour, with the `playhead` as its thumb.

**The amounts.** Each slider runs from 0 to the player's own stored value rounded down to 50, read
every frame (`tagpu_share_stored`); a press or a drag on its run sets the amount under the pointer
in steps of 50, and an amount above the end, after spending, is held to it. A panel opens with both
amounts at 0.

**Send** asks the give and closes the panel. The give runs at the next draw, in the post-GUI
observer on the game thread (`tagpu_sharetick_draw`), where the SHARE dialog's OK, a GUI handler,
makes the same calls. It is refused, with the toast *Give refused: that player is out of the game*,
unless the recipient is at that moment one of the chips' allies with a unit alive, and both records
pass the transfer's own send test (`0x457050`, `0x456EE0`: an own index `+0x146` that is not 10,
and units alive `+0x144` unless none was ever created `+0x140`): the engine's transfer takes from
the giver before it looks at either. An ask runs only in the level it was made in, and Send gives
nothing, with the toast *Nothing given: giving is not available in this game*, while the post-GUI
observer is not installed. Each amount is held to 0..the giver's stored value then, and given
through the engine's own transfers, metal `0x464C60` and energy `0x464B30` with `send` = 1, from the
controlled record: the giver's peer debits, credits its own copy of the recipient's economy record,
and, for a remote recipient, sends the `0x16` that credits it on its peer; a local AI's record is
the one credited, and nothing is sent ([recorder](../engine/recorder.html), `0x464C60`). An AI
receives the full amount (*AI allies*). Each give is logged
(`share: gave 500 metal to record 1 (SHTICK2)`).

**When the ally goes.** A panel whose ally loses its chip (it leaves, or is no longer allied both
ways) closes with the toast *Nothing given: <name> is no longer an ally*, and one whose ally goes
out of the game with *Nothing given: <name> is out of the game*. When that was the last chip the
ribbon is no longer drawn and the panel closes without a toast (the log says `hud: the give panel
closes`), so a ribbon that comes back later comes back closed.

## AI allies

An AI ally is an ally like a human one, in a skirmish and in a network game (the owner's rule,
[the AI-allies plan](../shared-resources-ai-plan.html)). Three always-on fixes in `tagpu_patches.c`
make the engine treat it so, each through the engine's own code ([hook map](hook-map.html) §2.6a):

- **A hosted AI's numbers** (`ai_economy_send`). The players phase sends the viewed record's `0x28`
  every fourth 30-tick pass of a network game (`0x4655A1`); the call now also sends it for each AI
  the peer hosts, from the AI's own DirectPlay ID, and every other peer's receiver writes it into
  that AI's record as it does a human's. Without it a remote AI's stored values stayed at their
  level-start values.
- **A give in full** (`ai_untaxed`). The transfers credited a local AI half the amount at Easy and
  seven tenths at Medium (`0x464D3A`, `0x464C0A`); now the full amount, from every giver: the panel,
  the console's Give, the auto-share, the `0x16` on an AI's host.
- **The auto-share for every ally and every player** (`ai_autoshare`). The engine's auto-share gives
  to any record the giver allies, a local or remote AI as a human (its remote and human tests
  removed), runs in a skirmish (its network gate removed, and its caller `0x495490` calling it with
  the step's argument 0), and runs for the controlled record and then each AI the peer hosts, in
  record order, each with its own levels. The stock AI sets no level and so gives nothing; an AI
  that sets one shares as a human does. A puppet level runs none of it: the recording carries what
  the owner's auto-share did.

## Gaps

- A negative level, which only the console sets (SetShareMetal has no lower bound), puts the
  engine's tick left of the bar while this one is held at its left end.
- A replay shows neither this tick nor the ribbon, its viewer sitting in no player's record; it
  shows the engine's tick wherever the recorded level puts it.
- The palette entries 24 and 89 are the stock palette's; a mod with its own palette shows its own
  colours there.
- In a game without the contribution channel the ribbon shows the allies' stored values and
  storages only: no rates, no share marks, and the lamp's 2 % test alone.
- The fall-back in the middle of a game rests on the recorder's facts above: a surrender's final
  seal was seen to take its source out, but a channel failing on a peer that stays in the game, or
  this peer ceasing to apply it, was not forced in a test; only the channel refused from the start.
  That another peer's channel failure reaches this one is inferred: the failing peer closes its
  private session, which destroys its player there; when it hosted that session, it rests on
  DirectPlay's host migration, which is not verified.
- A give lands at the recipient's storage at most: what is above it is lost, as with the SHARE
  dialog. The panel shows the ally's room but does not cap the amount at it.
- A player who leaves loses its chip: the engine's player removal `0x452CC0` writes the record's
  controller `+0x73` to 0 (through `0x463C60`, `0x463C67`), its first dword to 0 (`0x452E67`) and
  its alliance row `+0x108..+0x112` to 0 (`0x452E8B..0x452EA5`), and each fails the chip's test.
  A player whose units are all gone, or one whose peer is lost before that removal runs, keeps
  its chip, drawn out of the game once it has no unit alive.
- A takeover in a skirmish loses what the recipient's `+0xEC` object holds when it is taken, a
  pending give's credit with the rest: the recording does not carry that object (its row in
  `replay-layout.tsv` is `object`), and the auto-share gives every 60 ticks.
- A remote AI's chip without the channel has no rates: the `0x28` carries stored values and
  storages, not income or spending.
- On GDI there is no ribbon.
