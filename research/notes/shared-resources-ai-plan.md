# Allies' resources with AI allies, in skirmish and multiplayer (the plan)

## Status

**Planned 2026-11-14, on the owner's ask that the allies' resources HUD work "in local and
multiplayer with AI". Nothing built. D1 is decided (2026-11-14: the tax goes); D2 to D5 wait on
the owner.** It extends
[the allies' resources plan](shared-resources-plan.md), whose four landings are on local `main`:
the share tick, the allies' ribbon, the give panel and the allies' rates
([share](modules/share.md)). Today all of it is drawn only in a network game, and the ribbon offers
only remote human allies.

The engine facts below were gathered 2026-11-14 by a disassembly sweep of the pristine exe, each
cited there; the sweep's full report is kept beside the prototype in `.data/sharehud/` (local).

## What changes for the player

- **Skirmish.** A computer player allied with you gets a chip under the top bar with its metal and
  energy, its income and spending, and the lamp; a press opens the give panel, and Send gives it
  resources.
- **Multiplayer.** An allied computer player gets a chip too, whether your machine hosts it or
  another player's does.
- Remote human allies keep the chip they have now.

## The engine

- **The transfers work outside a network game.** `0x464C60` (metal) and `0x464B30` (energy) with
  `send` = 1 debit the giver (`0x401260`, `0x401220`) and credit the recipient's `+0xEC` economy
  record (`+0x18` metal, `+0x00` energy) with no network test. Their senders `0x457050` / `0x456EE0`
  always call `0x451BC0`, which returns at `0x451C87` without the network bit and needs a type-3
  destination: in a skirmish nothing is sent, and nothing needs to be. The console's `Give` already
  gives to an AI in a skirmish this way.
- **An AI spends what it is given.** The economy pass `0x401360` runs for types 1 and 2 (gate
  `0x465539`) and folds the `+0xEC` record in with no type test (`0x40190E`): the give becomes stored
  value at the AI's next 30-tick pass, capped at its storage. The stock AI reads stored values when
  it builds (`0x40BB00`, from the AI block through `0x40BDB0`; metal makers `0x408670`).
- **Stock taxes a give to a local AI.** When the recipient's controller is 2 (local AI), the
  transfer credits 50 % at difficulty 0 and 70 % at difficulty 1 (`0x464D36`, `0x464C06`; the
  difficulty is `main+0x37EEE`, set by the skirmish screen's Difficulty gadget
  `0x47B919..0x47B9C6`); at 2 or more the full amount. The giver is debited the full amount
  either way. A give to an AI hosted on another peer is scaled on that peer, by its value.
- **Who is who.** A local AI: controller `+0x73` = 2, PlayerInfo `+0x94` = 2. An AI hosted on
  another peer: controller 3, `+0x94` = 2. A remote human: controller 3, `+0x94` = 1. In a skirmish
  `+0x04` is the seat index (`0x464381`); in a network game it is the player's DirectPlay ID, a
  hosted AI's its own (`0x451421`).
- **Skirmish alliances are both ways.** The seat fill (`0x47A82F..0x47A890`) marks `+0x108[j]` for
  every active seat j in the same group, AI rows included.
- **A remote AI's numbers.** No `0x28` refreshes a remote AI's stored values during play (the
  periodic sender `0x4655A1` runs for the viewed record only, `0x465565`): stock state holds its
  level-start values. The contribution channel publishes every controller-1 or -2 record, so each
  hosted AI's image arrives under its own DirectPlay ID (measured 2026-10-04 with one AI a peer,
  [the multiplayer slice](tadr-port/demo-recorder-multiplayer-slice.md)).
- **A remote AI's alliance toward you** reaches this peer only through its host's `0x23`
  (`0x454D2A`); the engine keeps "that player allies me" in this peer's mirror row `+0x113`
  (`0x452A75`) as well as in the remote record's row. Which of the two the chip test should read
  for a remote AI is landing 0's question.
- **Sharing.** The stock auto-share `0x457D30` runs only in a network game (`0x457D3D`) and gives
  only to remote humans (type 3 and `+0x94` = 1, `0x457DD6`, `0x457DDD`): never to an AI. No stock
  AI code calls a transfer: a stock AI never shares. Outside a network game the share bits and
  levels act on nothing; the bar's own tick still draws a level (`0x469133..`), which is cosmetic.
- **Replays.** A recording keeps every player record each tick, economy included, but not the
  `+0xEC` record's contents. A puppet holds every record at type 3, so its economy pass never runs
  and its stored values come from the images: a give replays from state, with no event
  [INFERRED]. A credit still pending in `+0xEC` (up to 30 ticks) would be lost by a takeover inside
  that window [INFERRED]. On a playback level only `0x464C60` is gated (`tagpu_puppet.c`), not
  `0x464B30`.

## The decisions (the owner's)

| # | Question | Recommendation | Why |
|---|---|---|---|
| D1 | The 50 %/70 % tax on a give to a local AI at difficulty 0 and 1 | **DECIDED 2026-11-14 (owner): remove it.** An AI receives the full amount at every difficulty, from every giver the transfer serves (the panel, the console's `Give`, the `0x16` receiver on an AI's host) | The owner's choice over keeping it with a line in the panel. It changes the engine's own rule, so it is a simulation change: a byte patch at the scale's two sites, always on, with the replay gate (landing 2) |
| D2 | An allied AI hosted on another peer, in a game without the contribution channel | **A chip with the name and the give panel, and no strips or numbers** (a "no data" cell) | Stock state holds only its level-start values, which would be wrong from the first minute; showing nothing is honest, and the give still works |
| D3 | Your share level with AI allies (the stock auto-share never gives to an AI, and does nothing in a skirmish) | **The tick stays a network-game control for human allies only, as stock; AI allies are given to by hand through the panel.** An Impure auto-share that includes AIs and runs in a skirmish is a later plan of its own | Sharing with AIs automatically is a new simulation rule (who gets what, every 60 ticks, recorded, deterministic per giver) — its own landing with the replay gate, and it reopens decision 9 of the parent plan (the overflow rule) |
| D4 | Skirmish | **The ribbon, the rates and the give panel, as in a network game**; the share tick is not offered (D3) | The owner's ask; every number of a local AI is exact in its own record, so a skirmish needs no channel |
| D5 | AIs giving to you | **Out of scope here.** The stock AI never gives; ImpAI may later, through the same transfer, and would be its own decision on its branch | ImpAI is unmerged (`impai-brain` already patches the transfers' type tests for ImpAI seats, its rows 13 and 14); "no cheating" is ImpAI's rule, and a give from a human is not a cheat |

## The numbers a chip shows

| The ally | Stored, storage | Income, spending | Share mark |
|---|---|---|---|
| A local AI (skirmish, or hosted by this peer) | its record (`+0x98`, `+0x8C`, `+0xA8`, `+0xA4`), exact | its record (`+0x9C`, `+0xA0`, `+0x90`, `+0x94`), exact | none (D3) |
| An AI hosted on another peer | the channel's image, by its DirectPlay ID; without the channel, none (D2) | the channel's image; without it, none | none (D3) |
| A remote human | as now: the channel's image, else the record (`0x28`) | as now | as now |

The bound stays the parent plan's: a value is data until bounded, every image passes the finite
and non-negative test, a record is read under the share module's lifetime.

## The bounds

- **A give**: as now — the recipient is, at the call, one of the chips' allies with a unit alive,
  and both records pass the transfer's send test; each amount is held to 0..the giver's stored
  value. What changes: the network gate of `player()` gives way to "a level in play, not a
  watcher", and the ally test admits controller 2 and remote AIs. The give still runs on the game
  thread, from the post-GUI observer, once a draw, in the level it was asked in.
- **A local AI's record** is read under the same lifetime as the controlled player's: the level
  open and not closing, the index bounded by the ten records.

## The landings

Each is a local landing on its own work branch; the replay gate runs once, when the feature lands
on main (CLAUDE.md). Every landing touches `tagpu/ddraw`; 2 and 3 widen who an engine write is made
for, so they take the `high` review.

**0. The open facts.** Which alliance row the chip test reads for a remote AI (its row, or the
mirror `+0x113`), measured with an AI hosted on each side; a skirmish AI's name at `+0x2B`;
`[0x506DBC]` in a skirmish; whether the giver's "requested" entry in `+0xEC` is subtracted again at
its next pass; the difficulty values' labels. Into the engine map.

**1. Chips for AI allies.** The ribbon in a skirmish and for AI allies in a network game: the ally
test by controller and `+0x94`, the numbers by the table above, the "no data" cell (D2), the
share tick still network-only. The packet and the mockup carry the ally's kind. Verified in a
skirmish with an allied stock AI, and in a two-player network game where each peer hosts an allied
AI, with and without the channel.

**2. Giving to an AI, untaxed.** The give panel for every chip, and the tax removed (D1): the
type-2 test before the scale (`0x464D36` metal, `0x464C06` energy) patched so the credit is the
amount the giver paid, at every difficulty — read the two sites in full first, and patch the test,
never the constants `0x4FD548`/`0x4FD540`, which may have other readers. Always on, in a `sim` file
with its `SIM_FEATURE`, a hook-map row and the engine map's entries for the two sites. The
`impai-brain` branch already exempts ImpAI seats at the same tests (its rows 13 and 14): whichever
lands second takes the other's patch into account, and with this one ImpAI's exemption has nothing
left to do. Verified: a skirmish give of 1000 metal to an AI at difficulty 0 raises its stored value
by 1000 (up to its storage) at its next economy pass, where stock raises it by 500; the console's
`Give` likewise; a network give to an AI hosted on the other peer arrives in full there; the replay
gate with a test group that gives to an AI (landing 3).

**3. The replay of a skirmish give.** A test group that gives to an AI in a skirmish (the console's
`Give`, the same transfer) and plays it as a puppet, sought and taken over — the pending `+0xEC`
credit is the case to prove or close. And `0x464B30` gated on a playback level, as `0x464C60` is,
if the playback can reach it.

Later, each its own decision: an Impure auto-share that includes AIs and runs in a skirmish (D3);
ImpAI giving to its allies (D5).

## Open

- The decisions D2 to D5.
- The mirror row `+0x113` for remote AIs (landing 0).
