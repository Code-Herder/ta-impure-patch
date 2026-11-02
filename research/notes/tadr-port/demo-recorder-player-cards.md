# The replay's players and Sight

The playback's model of each recorded player, and Sight, the choice of whose view the world is
drawn in, independently of the camera. Both live in `tagpu_replay.c` (`tagpu_replay_players`,
`tagpu_replay_sight_request`, `inc/tagpu_replay.h`). The controls that show and ask them are the
replay HUD's rows ([The replay HUD in game](../hud-in-game.md)): a row per recorded player, and
Sight, Jump and Follow on its hover strip. They were first the player cards of
[the S5 design](demo-recorder-s5-ui.md), `tagpu_cards.c`, which the replay HUD replaced and which
are deleted. Jump and Follow play a recording's camera stream
([the perspective plan's 4c-2](demo-recorder-perspective-streams.md#4c-2-as-built)).

## Recorded model

`tagpu_replay_players` reads the same `RecWorld` player images the puppet applier uses, including
the final image after EOF; in a multiplayer recording, the composed one. No resource value is
interpolated. Header seats are bounded by `REC_PLAYERS`, and each row, in the recording header's
order with defeated seats kept, includes the recorded name, energy and metal amounts, storage,
production and expense, the units-alive word, the commander-loss word, the alliance row, and
`pov`, whether a camera sample is held for the seat (Jump and Follow).

- **The colour** is a palette index: the middle pixel of the seat's logo frame in the engine's
  colour sequence `*(main+0x148DB)`, the logo bounded by the sequence's count, the frame decoded
  only when it is nonempty and at most 64 × 64 (`logo_colour`), taken once a playing for every
  seat (`seat_colours`) and shared with the overlays. A seat with no colour gets index 59.
- **Commander presence** (`commander_alive`) walks the recorded live unit images. Its commander
  test is the engine's: UnitName equals the owning side's commander name, case-insensitively.
  Unit, owner and definition IDs are bounded; sides must be below both the loaded count and the
  five records the engine owns. An unavailable definition produces unknown status, not a claim
  that the commander is absent. **Nothing reads it now**: the replay HUD's skull is the
  commander-loss word above 0, and its dimmed row the units alive at 0.

## Sight, independently of the camera

Sight queues a recorded seat; -1 queues the viewer's record 10, the full map. On the replay HUD,
Sight on a row asks that row's seat, and Sight on the row it is already set to, drawn pressed,
asks -1. A pending seek or timeline boundary has priority. The game-thread frame visit first
applies the display state for the current iteration, then handles Sight, including paused frames
and EOF.

Unit sight counters already belong to their individual players. Temporary death vision is the
part that depends on who was viewed when a unit died. `sight_change` opens a separate reader on
the same timeline and collects that seat's death history with the existing bounded `eye_visit`
window. A failed read cannot displace the playback reader's next image or events. Only after a
successful read does it drop the old temporary records through the engine's expiry, select the
new viewed seat and add the new records through the engine's sight root. It then rebuilds
minimap fog and radar contacts and runs expiry. When the new seat is the recording's own viewed
record, contact bits 8–10 are then taken from the image the host holds, by the seek's rule
(R14b in `tagpu_replay.c`): in that view a playing holds the recording's bits, where the pass
alone shows the present world until the next 30-tick pass. Past the recording's end nothing is
copied. Deaths expired at the host's current time are
omitted, including when the clock has advanced beyond EOF.

The order is load-bearing: removing old stamps precedes selecting the new view; new stamps
precede the fog/contact passes. The temporary-record count is bounded by the pool capacity before
any expiry is changed. At GameTime zero the operation requires no existing temporary records,
since the engine's strict-less-than expiry cannot remove one then. None of this changes the
camera, controlled seat, playback clock or recording file.

The resource bar follows the new seat: the rates from the next frame; the stored values ease
toward the seat's over the following frames (an eighth of the difference a frame, at least 1,
`0x468E7B..0x468F12`). The engine re-reads the bar's `+income −expense` rates only while the
viewed record's DisplayTimer `+0xF8` is behind GameTime, so a seat viewed earlier in the same game
second, or any seat after a seek, would keep the previous record's rates — for up to a game second
running, and for as long as the game is paused ([render.md](../engine/render.md), *The memo's
layout*). Right after it writes the viewed byte, `sight_change` lowers the new record's
DisplayTimer, when it is at or above GameTime, by whole steps of 30 to the last value of its phase
below GameTime (`bar_rates_due`), in the frame visit before the in-play draw call, so the draw that
follows re-reads that record's rates once and leaves the timer where the engine's own steps would;
at GameTime 0 it writes nothing, and the first tick's draw re-reads as for any record. The field is
display state: only that draw and the saver read it, and the recording drops it. A seek leaves
every record's timer at GameTime, so after a paused seek the rates stay the pre-seek ones until
the game runs, and so after a takeover that leaves the game paused and after a return to now,
which change the view around that write: the engine's rule, left for a later landing by the
owner's choice of 2026-11-01 ([render.md](../engine/render.md), *What the Sight store does not
cover*).

## Verification

The checks below were made through the player cards, which called the same `tagpu_replay.c`
entry points the replay HUD calls now; the HUD's own run is in
[The replay HUD in game](../hud-in-game.md), *Status*.

On the reference setup, a paused switch between seats and the full map kept GameTime at 476, and
the full map kept a separately verified camera at (1800, 1900).

A fresh `battle-los` recording passed the replay harness's live, puppet, round-trip and fresh-seek
legs, including the byte comparisons and radar picture. A separate playback opened in perspective
0 and sought to iteration 331. At paused GameTime 330, the full map followed by Sight 0 recreated
six death-vision records. Its frame dump compared equal to the frame dump before switching,
including player sight counters. The same comparison at GameTime 1900 (iteration 1901, the
fixture's LOS-mode change) also compared equal, with two death-vision records recreated. These
are perspective-preservation checks; they do not claim that every possible sequence of seat
switches was tested.

The full map and Sight 0 also both succeeded at GameTime zero after seeking to iteration 1.
Seeking again from zero requires a playback step, as the existing seek gate specifies. After the
recording ended at iteration 3990, both still worked while the host clock continued beyond EOF;
the model kept the final recorded resources, three defeated seats and one surviving unit without
a commander.

The harness's W lane clicks the replay HUD's Sight on the first row four times while paused,
after its seek — that row's seat, the full map, the seat and the full map again — checking the
viewed seat, the clock and the camera, and that a frame drew the viewed record's bar (the memo's
byte 0 its `+0x146`) with the record's four rates in the memo bit for bit; the bar's row could not
run when the seat's rates equal the full map's ([completeness](demo-recorder-completeness.md),
*The player's way in (W)*).
