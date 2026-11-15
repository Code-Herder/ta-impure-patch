# Allies' resources and sharing on the HUD (the plan)

## Status

**Designed 2026-11-11..14 with the owner, on a UI prototype and a design interview; the engine
facts and the transport were read the same week. All four landings are on local `main`
(2026-11-14): the tick, the ribbon, the give panel and the allies' rates; the replay gate passed
over the whole feature, its two-player game seating the humans allied so the ribbon reads the
channel.** In a network game Impure shows each ally's metal and energy under the top bar, lets the
player give resources to an ally from there, and makes the game's own share tick on the resource
bar the control for the player's share level. Stock offers the same through console commands and
the SHARE dialog only.

The prototype is `tools/sharehud/mockup/index.html` on the work branch (throwaway, not for main):
`?variant=B2b&look=kit&own=tick` is the chosen design, and its header comment lists the answers.
Its top bar is composed by `tools/wiki/mockup/bar.js` from the stock side pack, so it is the
game's bar pixel for pixel.

## The decisions

| # | Question | Decision | Why |
|---|---|---|---|
| 1 | The allies' element | **A ribbon of chips** under the top bar, right-aligned, one per ally: name, colour slot, a metal and an energy cell, each with income and spending as numbers and the storage as a strip along the cell's bottom edge, and a mark at that ally's share level | Chosen over a column, a table and bars with numbers beside them (prototype variants). Nothing is written on the resource colour |
| 2 | The look | **The replay HUD's kit** (`art/hud/parts`): `lanes_plate` the chip, `pick_row` the cells, `picker_one` the give panel, `chip_face` its button. New parts: a horizontal slider track and thumb, and the lamp | One kit across Impure's HUD. A distressed-rust repaint was tried and dropped |
| 3 | A stalling ally | **A square lamp on every chip, dark, that blinks** lit when the ally's stored metal or energy is under 2 % of its storage while spending exceeds income. With no rates known (decision 8), the 2 % test alone | Chosen from eight looks (glow, frame, hatching, …) |
| 4 | Who gets a chip | **Mutual allies only**: the local player allies them and they ally the local player | The engine's relation is one-way (*The engine*, below); a chip for a player who has not allied back would offer a give to someone who shares nothing |
| 5 | The top bar | **The game's own, untouched** but for the tick | The owner's choice; a taller marker collides with the storage numbers the engine prints at the bar's ends |
| 6 | The player's share level | **Drag the game's tick on its bar, or click the bar.** The far right end is sharing off; anywhere else is sharing on at that level. Off at the start of every game. While the pointer is on the top bar the level is written under the bar | The engine has a switch and a level; the player sees one control. Chosen over a rail under the bar, a drop-down and a chip of the player's own |
| 7 | Giving | **Click a chip: a panel with two sliders, 0 to what the player holds, and Send.** The chip takes no click once that ally is dead or gone | The engine debits a give to a dead or departed player and credits nobody |
| 8 | Where the allies' numbers come from | **The recorder's contribution channel**, which already carries every Impure player's whole player record to every Impure peer each tick (*The transport*, below). No new message. In a game the channel does not run, every chip shows the storage strips only, at the engine's own pace | Stock state holds no rates and no share levels for a remote player. Settled 2026-11-14 by reading the channel; the interview's "a new Impure message, to allies only" is replaced, since the channel already sends the record to every Impure peer, enemies included |
| 9 | The overflow rule | **Stock's, unchanged, in these landings** | A better rule is a simulation change with its own replay work, and a landing of its own |
| 10 | More allies than fit | **Wrap to a second row**; chips keep their size | |
| 11 | Skirmish, watchers, replays | **Nothing is drawn** | The engine shares nothing outside a network game; the replay HUD has its own statistics; watchers can follow |
| 12 | Scale, sides, mods | **The HUD scale setting applies.** The tick's place comes from the running side's own `metalbar` and `energybar` rects. A side without them gets no tick and no ribbon | No list of mods |

## The engine

DISASSEMBLED 2026-11-14 from `pristine/TotalA.exe.pristine`, beyond what the engine map's
[recorder](engine/recorder.md), [units](engine/units.md) and [network](engine/network.md) pages
already hold. Landing 0 moves these into the map's entries; until then this section is the record.

- **A remote player's record has no rates.** Production and expense (`+0x90`, `+0x94`, `+0x9C`,
  `+0xA0`) have two writers: the economy pass `0x401360`, which runs for record types 1 and 2
  (gate `0x465539`), and the zeroing in `0x464700`. The `0x28` message carries stored, the two
  storages, kills and losses and six lifetime totals, goes to every remote human every 120 ticks
  (every fourth 30-tick pass, `0x4655A1`), and is ignored from a sender whose end-of-game `0x28`
  has arrived (`+0x129 + sender`, set at `0x45768A`).
- **The share bits travel, the levels do not.** `0x450F90` broadcasts the whole PlayerInfo as
  message `0x20`; the receiver `0x454874` copies it over the remote record's. The levels `+0xE4`
  and `+0xE8` are written by SetShareMetal and SetShareEnergy alone: an absolute integer capped
  at the storage, no lower bound, network games only, nothing sent.
- **The auto-share `0x457D30`** runs every 60 ticks for the controlled player. For a resource
  whose bit is set and whose stored value is above the level it picks **one** recipient: the
  highest-index remote human the giver allies whose stored value is below the giver's. The amount
  is the excess times ⅓ for metal (`0x4FD4B8`) and ½ for energy (`0x4FD4BC`), capped at the
  recipient's room; a full recipient gets nothing and nobody else is tried. Level 0 shares
  everything. Its modulo-450 branch sends the map to allies.
- **Alliance is one-way.** `player+0x108[j]` is "this player allies j" and `+0x113[j]` its mirror.
  The ALLIES dialog (`0x4478B0`, `ALLIES.GUI`) toggles the local row and `0x452960` sends `0x23`
  to the other party's peer alone (receiver `0x454B64`). So a peer knows its own row and who
  allies it, and nothing of an alliance between two other players. There is no helper: every
  reader tests the array.
- **A give is the engine's transfer.** `0x464C60(from, to, amount, 1)` for metal and `0x464B30`
  for energy debit the giver, send a 17-byte `0x16` to the recipient's peer and return; that peer
  credits with `send` = 0. The console's `Give` and the SHARE dialog (`0x4933E0`, `SHARE.GUI`)
  call the same two.
- **What these paths do not check.** The transfers test their indices against 10 only. `Give`
  has no alliance test and no sign test: a negative amount takes from the recipient. The `0x16`
  receiver checks neither the amount nor the sender. `0x20` can overwrite another player's
  PlayerInfo, and a `0x23` with its force flag makes the local player ally someone. These are
  stock's; Impure's own calls are bounded (*The bounds*, below) and the receivers stay stock's
  here.

Not established: whether a player can view a non-local record during play (that peer then stops
sending `0x28`); the lobby's initial share bits; that key code `0x68`, which opens SHARE, is `h`
[INFERRED].

## The transport

Read 2026-11-14 in `tagpu_recorder.c` and the [multiplayer slice](tadr-port/demo-recorder-multiplayer-slice.md),
*Contribution transport checkpoint*.

- **Telling an Impure peer from a stock one** is already done: an Impure player's DirectPlay
  player data is the engine's 21 bytes, then a public key, then the mod fingerprint
  (`mp_create_player`); `mp_public_key` and `tagpu_rec_player_print` read them back off the
  record DirectPlay keeps for a native ID.
- **The channel runs only when every seat runs Impure.** `mp_capture` opens it once, at the first
  capture point of a network game, and gives up with "no contributions" when any active seat's
  player data has no key. A stock host also refuses an Impure joiner outright (it takes exactly
  21 bytes, `0x454315`). So a game has the channel for every ally or for none, never per ally.
- **What it carries.** At every capture point, each local human and hosted AI publishes its
  normalized player image (`player_image`, `REC_PLAYER_IMAGE` = 0x208 bytes: the record's
  0x14B bytes, then PlayerInfo at `REC_INFO_AT` = 0x14C) as a full image and then changed-word
  deltas, recording or not. The image keeps the economy, so production and expense (`+0x90`,
  `+0x94`, `+0x9C`, `+0xA0`), the share levels (`+0xE4`, `+0xE8`) and the share bits (PlayerInfo
  `+0x97`) all arrive. `mp_apply` puts each into the authoritative cache `s_mp_players`, apart from
  the observed world, where `tagpu_recmp_players_get(origin)` returns the image with its source
  and applied ticks.
- **When it stops.** The channel can fail, and a peer can stop consuming while still sending
  (the latch in *Channel failure handling*), and one source's channel can fail while this one
  runs on. A chip falls back to stock state, as in a game without the channel, when this peer's
  channel failed (the recorder ends its cache), when this peer stopped applying it (the consuming
  latch), or when that source no longer contributes (its final seal, or its private player gone
  from the session: `tagpu_recmp_source_live`). The fall-back is by those facts, not by how old
  an image is: a source publishes only what changed. That a failing peer's player leaves the
  others' sessions is inferred, and for the private session's host rests on host migration.
- **Threads.** The cache is written and read on the game thread. The HUD draws on the render
  thread, so the numbers reach it in the frame packet, never by a read of the cache.
- **The recorder.** The ribbon is not drawn in a replay (decision 11), and every byte the HUD
  writes, the share bits and levels, is a player-record byte the recorder already keeps; a give is
  the engine's own transfer. Nothing in the recorder changes unless `make` or the replay harness
  names it.

## Related work: allied sharing

The allied sharing branch (`worktree-allied_share`, not on `main` when this was written; its note is
`modules/allied-sharing.md` there) shares sight, radar contacts and the explored map between allies.
Read 2026-11-14 for what this plan can take from it:

- **The same alliance rule.** It counts an alliance only when held both ways and reads the rows
  `+0x108` as this plan does (decision 4), and it records the same network fact: the rows between
  two other players can be stale on a peer, since `0x23` goes to its target alone. The engine map on
  `main` already has the rows' entry (units, `+0x108`/`+0x113`), and `main` already has two
  mutual-alliance tests to copy rather than a third design (`tagpu_hudstate.c`'s teams,
  `tagpu_rangeown.c`'s `allied`). Its own test, `tagpu_allysight_shares(owner, viewer)`, is the one
  to call once that branch lands.
- **Its seating of allies is skirmish only.** `ally_group` in a scenario and `tacli --player`'s
  sixth field write the skirmish key `Player<N>AllyGroup`. A network game's alliances come from the
  battle room's teams or the in-game ALLIES dialog, and no tool drove either; that branch's own
  two-player allied network game was never run for the same reason. Landing 1 built it:
  **`mp_lobby.sh --allies`** presses every other row's Ally button on every peer, so the game starts
  with everyone allied both ways (one team for all is refused by the battle room), and allied
  sharing's network check can use it too.
- **No overlap in what is written.** Allied sharing hooks the sight, radar and minimap sites and
  ignores the ShareRadar and ShareLOS bits; this plan writes the ShareMetal and ShareEnergy bits and
  the share levels and hooks nothing in the simulation. Whichever lands second merges `main` first.

## The bounds

Every value Impure passes to the engine or reads from another peer is data until it is bounded.

- **A give**: the recipient index is one of the ribbon's chips, which exist only for an active
  remote record under index 10 that passes decision 4's test on the game thread at the moment of
  the call; the amount is clamped to 0..the giver's stored value there. The call is made on the
  game thread.
- **The share level**: clamped to 0..storage before the write; the share bit and its `0x20`
  broadcast change only when the switch changes, never on each step of a drag.
- **An ally's numbers from the channel**: shown, never fed to the simulation. The image is the one
  the cache holds for the ally's native ID, already matched to its seat by the channel; a rate that
  is not finite, or a level outside 0..that image's storage, is not drawn.

## The landings

Each is one finished unit by the project's landing rules. Landings 1 to 4 touch `tagpu/ddraw`; 1 and 3
write engine state and 4 passes game-thread state to the render thread, so each takes the `high`
review. Landings 2 to 4 are local landings on the work branch; the replay gate runs once, when the
finished feature lands on main (CLAUDE.md, *Commit freely, land locally, merge to main once*).
Landing 1 reached `main` before that rule (`b621affd..66cf8042`), without the replay gate after main
was merged in (its own branch had passed the gate's 17 test groups), so the landing on main runs
`tools/replay/tareplay.py landing --base b621affd`, which covers landing 1 too.

**0. The facts.** *The engine*, above, goes into the engine map's entries.

**1. The tick.** No GUI screen owns the top bar, so the tick is drawn and hit-tested by the DLL
as the Impure Config sprocket and the live rewind button already are ([engine UI](engine/ui.md),
*What owns the top bar*; [the HUD in game](hud-in-game.md), *Live rewind*). The click writes what
the engine's share commands write, on the game thread, and calls their broadcast `0x450F90`, so the
bit's `0x20` is the engine's; the commands themselves are not called, since each prints a chat line
and a drag would print one a frame ([share](modules/share.md)). The share tick drawn at every level, in three states (holding more than the
level, less, sharing off), dragged or set by a click on the bar, with the level written under the
bar while the pointer is on the top bar. It writes the share bit and level of the controlled
player. Verified in a two-player allied network game, seated by the new `mp_lobby.sh --allies`
(*Related work*): the stock auto-share gives above the level and not below. **Built**: the module is
[share](modules/share.md).

**2. The ribbon from stock state.** Chips for mutual allies with the storage strips, the wrap,
the lamp on the 2 % test, the kit parts and the lamp's two new parts, at every HUD scale. No rates,
no share marks. **Built**: the HUD's third mode, its layout ported from the mockup's and held to it
by `laycheck`. Verified in a three-player
allied network game on Vulkan: each peer showed its two allies' chips in their colours, the game's
pixels matching the mockup drawn from the game's own state dump (mean 1.5/255 inside the chips);
an ally that gave away everything through SHARE blinked its lamp on both other peers; one player
dropping one alliance in ALLIES took the chip away on both sides of it and kept the third peer's
two; a device click on a chip kept the commander selected where one on the ground cleared it.

**3. The give panel.** The panel, its sliders and Send, with the bounds above; the chip's
disabled state. Verified by the recipient's stored value on the other peer, and by a give that is
refused to a player who left. **Built**: the panel under the chip, its sliders made of the live
strip's groove and playhead filled in the resource's colour (no new parts), Send a stats-window chip
face; the give runs from the share tick's post-GUI draw through the engine's two transfers
([share](modules/share.md), *The give panel*). Verified in a three-player allied network game on
Vulkan: a give of all 1000 of one peer's metal took it to 1 there, and the other peer's record of
it to 7 at its next report; 500 back raised the first peer's own stored metal from 25 to 531 on its
own peer; slider presses set 500, 400 and 650 by the run's fraction in steps of 50; a panel open
on an ally with 400 energy asked closed when that ally surrendered, with the toast, and nothing was
given. The give's own refusal, for an ally that goes between Send and the next draw, was not
reached: the panel closes first, every frame, and the refusal stands behind it as the bound. The
disabled chip was drawn by `laycheck` only: a surrender removes the player at once, so no ally
with no units stayed in the game to show it. After the review's fixes, a two-player game gave 500 metal again,
and the panel open on the player's only ally closed, once, when that ally surrendered and the
ribbon went with it.

**4. The allies' rates.** The chips' numbers and share marks and the lamp's full rule, from the
channel's cache through the frame packet, with the fall-back to stock state when the cache stops.
Verified in a three-player game of Impure peers, and with the channel refused (the test lever
`tagpu_modprint_stock.on` on one peer). **Built**: the recorder answers an Impure player's economy
out of its player cache while the channel runs and this peer applies it (`tagpu_rec_mp_economy`);
the ribbon takes it per ally under the bound above and carries it in the HUD record; the cells'
numbers and the marks are the mockup's, held by `laycheck` with and without the channel. Verified
in a three-player allied network game of Impure peers on Vulkan: another peer's chip for the host
read the host's own `+0x9C`/`+0xA0`/`+0x90`/`+0x94` at every sample while a solar and then a metal
maker were built (energy income 25, then 45, spending 79.1, then 60); the host's lamp lit while its
energy was under 20 and spending exceeded income, and went out as it refilled; a peer that pressed
its metal bar showed its mark at 55 % (550 of 1000) on the others' chips. With
`tagpu_modprint_stock.on` on one peer every peer logged "no contributions" and the chips showed the
strips alone, with no numbers and no mark for a peer that shares. After the review, the chips
read a source's numbers only while the channel says it still contributes; a surrender was seen to
take its source out on both other peers. A channel failing mid-game on a peer that stays was not
forced.

Later, each its own decision: a better overflow rule (the neediest allies, as far as they have
room); watchers; a replay's view of the ribbon.

## Open

- The prototype was judged in stills and by the owner's hand at one HUD scale with four allies
  and the ARM bar. The wrap and nine allies are drawn by the replay HUD's mockup
  (`tools/hudart/mockup/?mode=allies&na=9`) and not yet judged by the owner.
- The lamp's parts are made from the 1K test painting (`tools/hudart/lamp.py`); a repaint at its
  final size would replace them.
