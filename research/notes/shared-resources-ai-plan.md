# Allies' resources with AI allies, in skirmish and multiplayer (the plan)

## Status

**Planned 2026-11-14, on the owner's ask that the allies' resources HUD work "in local and
multiplayer with AI". Every decision is taken (2026-11-14), all by one rule of the owner's: an AI
ally behaves exactly as a human ally does, in skirmish and in multiplayer, and the simulation
changes where it must. Built on the work branch `worktree-multi_shared_view`: landings 0 and 1
verified; 2 to 7 built, reviewed (`high`) and verified in a skirmish and a two-peer network game
(below each).** It extends
[the allies' resources plan](shared-resources-plan.md), whose four landings are on local `main`:
the share tick, the allies' ribbon, the give panel and the allies' rates
([share](modules/share.md)). Today all of it is drawn only in a network game, and the ribbon offers
only remote human allies.

The engine facts below were gathered 2026-11-14 by a disassembly sweep of the pristine exe, each
cited there; the sweep's full report is kept beside the prototype in `.data/sharehud/` (local).

## What changes for the player

An allied computer player is an ally like a human one, in a skirmish and in a network game, whichever
machine hosts it:

- its chip under the top bar: metal and energy, income and spending, its share marks, the lamp;
- the give panel, which gives it the full amount;
- your share levels on the resource bar, in a skirmish too, and your overflow above them shared
  with it as with a human ally;
- its own share levels act as a human's do: whatever it has set, its overflow goes to its allies.
  The stock AI sets none, so it shares nothing; an AI that sets one (ImpAI may) shares.

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
- **Sharing.** The stock auto-share `0x457D30(giver)` runs only in a network game (`0x457D3D`) and gives
  only to remote humans (type 3 and `+0x94` = 1, `0x457DD6`, `0x457DDD`): never to an AI. It is
  one-sided and gives to one recipient a pass: the highest-numbered record the giver's own row
  `+0x108` allies holding less than the giver, a third of the metal and half the energy above the
  level, capped at the recipient's room (landing 0; [recorder](engine/recorder.md)). Its caller
  `0x495490` calls it only on the step's network path (`0x49560B`), so a skirmish never reaches it. No stock
  AI code calls a transfer: a stock AI never shares. Outside a network game the share bits and
  levels act on nothing; the bar's own tick still draws a level (`0x469133..`), which is cosmetic.
- **Replays.** A recording keeps every player record each tick, economy included, but not the
  `+0xEC` record's contents. A puppet holds every record at type 3, so its economy pass never runs
  and its stored values come from the images: a give replays from state, with no event
  [INFERRED]. A credit still pending in `+0xEC` (up to 30 ticks) would be lost by a takeover inside
  that window [INFERRED]. On a playback level only `0x464C60` is gated (`tagpu_puppet.c`), not
  `0x464B30`.

## The decisions (the owner's)

**The rule (the owner, 2026-11-14): with an AI ally, the behaviour is the same as with a human
ally, in skirmish and in multiplayer. If the simulation has to change, it changes.** D3 to D5
follow from it.

| # | Question | Recommendation | Why |
|---|---|---|---|
| D1 | The 50 %/70 % tax on a give to a local AI at difficulty 0 and 1 | **DECIDED 2026-11-14 (owner): remove it.** An AI receives the full amount at every difficulty, from every giver the transfer serves (the panel, the console's `Give`, the `0x16` receiver on an AI's host) | The owner's choice over keeping it with a line in the panel. It changes the engine's own rule, so it is a simulation change: a byte patch at the scale's two sites, always on, with the replay gate (landing 4) |
| D2 | An allied AI hosted on another peer, in a game without the contribution channel | **DECIDED 2026-11-14 (owner): its host sends its numbers.** Impure on the AI's host sends the engine's own `0x28` for every AI it hosts, at stock's cadence (landing 3), so every peer holds that AI's live stored values and storages in its record. If the receiver mishandles it, that is fixed — the owner ruled out any design where it does not work. Every peer runs Impure (the owner, 2026-11-14), so the receiver is always ours to fix | The engine already carries a player's numbers this way; it only never sends them for a hosted AI (`0x4655A1` serves the viewed record alone). No "no data" chip: the numbers are always live |
| D3 | Your share level with AI allies (the stock auto-share never gives to an AI, and does nothing in a skirmish) | **DECIDED by the rule: the engine's auto-share gives to AI allies, in a skirmish too.** The tests that leave them out — the network gate `0x457D44`, the remote and human tests of both resources (`0x457DD8`, `0x457DE4`, `0x457EF6`, `0x457F02`) — are removed, and its caller runs it in a skirmish (`0x49560B`) (landing 5) | A simulation change, taken through the engine's own auto-share rather than one of ours: the same one recipient a pass, now chosen among AI and human allies alike |
| D4 | Skirmish | **DECIDED by the rule: everything a network game has** — the ribbon, the rates, the give panel and the share tick | A local AI's numbers are exact in its own record, so a skirmish needs no channel; the tick acts there through D3 |
| D5 | AIs giving to you | **DECIDED by the rule: an AI's share levels act as a human's.** The auto-share runs for every player record a machine controls, AIs included, not only for `main+0x2A42` (landing 6). What level an AI sets is the AI's decision: the stock AI sets none and so gives nothing; ImpAI may set one, on its own branch, and its "no cheating" rule is untouched — sharing is a rule every player has | `impai-brain` already patches the transfers' type tests for ImpAI seats (its rows 13 and 14); whichever lands second folds in the other |

## The numbers a chip shows

| The ally | Stored, storage | Income, spending | Share mark |
|---|---|---|---|
| A local AI (skirmish, or hosted by this peer) | its record (`+0x98`, `+0x8C`, `+0xA8`, `+0xA4`), exact | its record (`+0x9C`, `+0xA0`, `+0x90`, `+0x94`), exact | its record (`+0xE4`, `+0xE8`, PlayerInfo `+0x97` bits 2 and 4), as for the player |
| An AI hosted on another peer | the channel's image, by its DirectPlay ID; without the channel, its record, which its host's `0x28` keeps live (D2) | the channel's image; without it, from the `0x28`'s fields if it carries them (landing 0), else derived from successive stored values — landing 0 decides which | the channel's image, as for a human; without it, its record (D2's `0x28` carries what it carries — landing 0) |
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
on main (CLAUDE.md). Every landing touches `tagpu/ddraw`; 3 to 7 widen who an engine write or an
engine message is made for, so they take the `high` review.

**0. The open facts.** The `0x28` receiver: how it finds the record it writes (the sender's
DirectPlay ID, a row field in the payload?) and which fields it writes — read in full, then
measured with an AI hosted on each side. And which alliance row the chip test reads for a remote AI (its row, or the
mirror `+0x113`), measured with an AI hosted on each side; a skirmish AI's name at `+0x2B`;
`[0x506DBC]` in a skirmish; whether the giver's "requested" entry in `+0xEC` is subtracted again at
its next pass; the difficulty values' labels. For the auto-share: whether its caller
`0x49563A` runs in a skirmish at all (its step argument and `[0x506DBC]`), how its overflow rule
divides among recipients, and what it reads of the giver beyond `main+0x2A42`, so it can run for
another record. Into the engine map.

**1. Only Impure players join.** The host refuses a joiner that sends no fingerprint tail — stock's
21 bytes of `CreatePlayer` data, or an older Impure's 301 — rather than taking it and comparing it
with nobody (`modules/mods.md` *The gate*), and prints in the battle room, for the host, a chat line
naming the refused player and why: "<name> was refused: not running Impure" or "… running an older
Impure, update it". The refusal is the host's own `CreatePlayer` stub, where today's code takes the
21 or 301 bytes; the line uses the gate's chat-line path. A stock or older joiner sees what an older
build's refusal shows today, Select Game with no message, since no code of ours runs there to say
more. Verified on Wine: a joiner with `tagpu_modprint_stock.on` (stock's 21 bytes through Impure)
and one on an older Impure build are both refused, the host's room shows each line once, and a
current joiner still joins; the compatibility suite's network rows still pass. **Built 2026-11-15**: the host refuses both and says *JOIN refused: not running Impure* / *… older Impure build* once each (`modmp.py --only stock|older`, v0.5's release DLL as the older build); a current managed joiner and a drop-in joiner still start (`--only dropin`); the compatibility suite runs at the landing on main.

**2. Chips for AI allies.** The ribbon in a skirmish and for AI allies in a network game: the ally
test by controller and `+0x94`, the numbers by the table above, the share marks of an AI. The packet and the mockup carry the ally's kind. Verified in a
skirmish with an allied stock AI, and in a two-player network game where each peer hosts an allied
AI, with and without the channel. **Verified 2026-11-15** (Wine, Vulkan): in a skirmish the AI's chip
carries its record's rates and stored values, beside the live rewind's idle button; in a network game
where one peer's AI is allied to both humans, each peer shows it (a local AI on its host, a remote AI
on the other) with its rates from the channel, and its lamp lit while it stalled (metal 0, spending
38.6 over income 3.3). An AI is allied in a network game only by the battle room's team icons
(`TEAMICONS<n>`): the room's ALLY gadget and the in-game ALLIES dialog are inactive on an AI's row.
Not run: a network game without the channel.

**3. The host sends its AIs' numbers.** On a peer that hosts an AI (controller 2 in a network
game), the engine's `0x28` sender `0x4573D0` called for each hosted AI record at stock's cadence (the
fourth 30-tick pass, as `0x4655A1` does for the viewed record), from the game thread. Whatever
landing 0 finds wrong at the receiver is fixed here, at the receiver. Every peer runs Impure, so
there is no stock receiver to allow for. A new engine message on the wire takes the `high` review;
the recorder's two-player game, with an AI on each peer, covers it in the replay gate. Verified:
each peer, watching the AI hosted by the other, sees its stored values move with the host's own
within a cadence, with the channel running and with it refused. **Verified 2026-11-15** with the
channel running: each peer's record of the other's AI held that AI's stored values (197 against its
host's 198 metal, 4000 ticks into the game), and each human's record kept its own (1000 against
its AI's 197), so a receiver files the AI's `0x28` under the AI. Not run: the channel refused.

**4. Giving to an AI, untaxed.** The give panel for every chip, and the tax removed (D1): the
type-2 test before the scale (`0x464D36` metal, `0x464C06` energy) patched so the credit is the
amount the giver paid, at every difficulty — read the two sites in full first, and patch the test,
never the constants `0x4FD548`/`0x4FD540`, which may have other readers. Always on, in a `sim` file
with its `SIM_FEATURE`, a hook-map row and the engine map's entries for the two sites. The
`impai-brain` branch already exempts ImpAI seats at the same tests (its rows 13 and 14): whichever
lands second takes the other's patch into account, and with this one ImpAI's exemption has nothing
left to do. Verified: a skirmish give of 1000 metal to an AI at difficulty 0 raises its stored value
by 1000 (up to its storage) at its next economy pass, where stock raises it by 500; the console's
`Give` likewise; a network give to an AI hosted on the other peer arrives in full there; the replay
gate with a test group that gives to an AI (landing 7). **Verified 2026-11-15**: at Easy the give
panel's 250 metal to the skirmish AI raised it by 250 at its next pass and its auto-share credit
filled it to its storage; from the other peer's panel 100 metal reached a remote AI whole (at
difficulty 2, where stock does not scale either). Not run: the console's `Give`.

**5. Your auto-share reaches AI allies, in a skirmish too.** The auto-share's three tests patched
(D3): the network gate, so it runs in a skirmish (and, if landing 0 finds its caller does not run
there, the caller's gate too); the remote and human tests, so a local AI and a remote AI are
recipients like a remote human, under the same mutual-alliance and unit-alive tests. The share tick
is offered in a skirmish (D4). Always on, a `sim` file, hook-map rows, engine-map entries.
Verified: in a skirmish with an allied AI, your overflow above your level reaches it each sharing
pass; in a network game with an AI and a human ally, it is split as the overflow rule says; the
replay gate (landing 7). **Verified 2026-11-15**: in a skirmish at Easy, a level of 500 gave the AI
its whole room each pass; in a network game a peer's level of 300 gave the remote AI a third of the
overflow (233, then 156), credited on its host. The engine gives to one recipient a pass, so there is
no split.

**6. Every player's auto-share.** The auto-share run for each record its peer controls — the
player and every AI it hosts — not only `main+0x2A42`, each with its own levels and its own
allies; in a network game the gives to remote recipients travel as the player's do (`0x16`).
Verified with an AI whose share levels are set by hand (`tacli` writes `+0xE4`/`+0xE8` and the
`+0x97` bits, since the stock AI sets none): its overflow reaches its allies, human and AI, in a
skirmish and in a network game. **Verified 2026-11-15**: in a skirmish the AI at level 200 gave the
player a third of its metal overflow (264) and half its energy's; in a network game the host's AI
sharing all its metal gave the remote human 69 of its 205; an unallied AI received nothing.

**7. The replay of sharing with AIs.** A test group in a skirmish that gives to an AI (the console's
`Give`, the same transfer), with your level set and an AI's set by hand so both auto-shares run, and
plays it as a puppet, sought and taken over — the pending `+0xEC`
credit is the case to prove or close. And `0x464B30` gated on a playback level, as `0x464C60` is,
if the playback can reach it. **Built**: the `ai-share` test group (Two Continents, you and an
allied AI sharing from 30, a third player apart; taken over at 905, just after a sharing pass).
Nothing on a playback reaches `0x464B30`: the typed command is gated, a playback runs no network
pump, and `ais_share_every` refuses a puppet level. The pending credit is not closed: the recording
does not carry the `+0xEC` object at all (its row in `replay-layout.tsv` is `object`), so a takeover
inside a pass's window loses whatever it holds, a give's credit with the rest — a gap of the
recorder's that sharing in a skirmish now meets every 60 ticks; closing it means recording the
object, a format change, which is the owner's call (*Open*).

Not here: ImpAI choosing its own share levels, on its branch (D5).

## Open

- The `0x28` receiver, for D2 (landing 0).
- Every peer runs Impure: the owner's rule, made true by landing 1 (2026-11-14, the owner: refuse
  a joiner without Impure and tell the host in the lobby).
- The mirror row `+0x113` for remote AIs (landing 0): the chip test reads it.
- **The `+0xEC` object at a takeover** (landing 7): carried by the recording (a format change), or
  kept as the recorder's documented gap — the owner's decision.
- The Deathmatch respawn's own type-2 credit (`0x465437..0x4654CF`) keeps the difficulty scale: it is
  not a give, and D1 names gives.
