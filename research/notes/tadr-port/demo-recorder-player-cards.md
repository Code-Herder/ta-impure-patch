# Replay player cards

Implemented and runtime-validated, including two independent high-effort reviews and their
frame-boundary corrections. This is the card portion
of [the S5 design](demo-recorder-s5-ui.md). Camera streams and the Overlays panel remain separate
work. The existing recording format has no camera stream, so Jump, Follow and Free cam are
visibly disabled. Hovering a camera control explains “Camera was not recorded.”

## Layout and input

`tagpu_cards.c` is the dock's game-thread companion. At HUD 1× its panel is 206 pixels wide,
8 pixels from the right edge, starts at y=40 and ends 8 pixels above the dock. Full map and Free
cam occupy a fixed 32-pixel footer. Cards are 70 pixels high with 4-pixel gaps, or 54 pixels high
when more than four players participate or the normal cards do not all fit. Scrolling moves
whole cards, with a visible indicator for the portion in view. Defeated seats remain in header
order. Names and numbers use the engine's FNT renderer; text size does not shrink with the list.

The engine serves only the top GUI screen and treats a screen as a rectangle. One GUI spanning
both dock and cards would also claim the intervening world; two separate GUI screens would leave
one without input. The cards therefore share the dock strip's post-GUI drawing and paired-click
path. Their immutable GAF frames use the supplied retail dock texture and Jump/Sight/Follow
icons. The engine's current font is measured and drawn with its foreground/background colours
saved and restored. Header text is restricted to printable ASCII before the engine indexes its
font table. Labels are clipped by measured width.

The cards draw only while DOCK.GUI is topmost. Hit tests also compare its stored identity with
`main+0x531`, so a modal takes input immediately, before the next model refresh. A consumed
press owns its release, even after closing the panel; a cancelled press cannot invoke Sight.
Wheel messages over the cards are handled before either the world zoom or the GUI list wheel.
Config replaces the dock through the existing screen slot; teardown clears the card region.

## Recorded model

`tagpu_replay_players` reads the same `RecWorld` player images the puppet applier uses, including
the final image after EOF. No resource value is interpolated. Header seats are bounded by
`REC_PLAYERS`, and each row includes the recorded name, colour, energy/metal amounts and
production/expense, the units-alive word and commander-loss word. The normal card also reports
commander presence by walking the recorded live unit images. Its commander test is the engine's:
UnitName equals the owning side's commander name, case-insensitively. Unit, owner and definition
IDs are bounded; sides must be below both the loaded count and the five records the engine owns.
An unavailable definition produces unknown status, not a claim that the commander is absent.

## Sight, independently of the camera

Sight queues a recorded seat; Full map queues the viewer's record 10. A pending seek
or timeline boundary has priority. The game-thread frame visit first applies the display state
for the current iteration, then handles Sight, including paused frames and EOF.

Unit sight counters already belong to their individual players. Temporary death vision is the
part that depends on who was viewed when a unit died. `sight_change` opens a separate reader on
the same timeline and collects that seat's death history with the existing bounded `eye_visit`
window. A failed read cannot displace the playback reader's next image or events. Only after a
successful read does it drop the old temporary records through the engine's expiry, select the
new viewed seat and add the new records through the engine's sight root. It then rebuilds
minimap fog and radar contacts and runs expiry. Deaths expired at the host's current time are
omitted, including when the clock has advanced beyond EOF.

The order is load-bearing: removing old stamps precedes selecting the new view; new stamps
precede the fog/contact passes. The temporary-record count is bounded by the pool capacity before
any expiry is changed. At GameTime zero the operation requires no existing temporary records,
since the engine's strict-less-than expiry cannot remove one then. None of this changes the
camera, controlled seat, playback clock or recording file.

## HUD scaling and frame ordering

`tagpu_hud_cards_map` computes the engine and device rectangles from surface size and scale.
The panel grows up and left about its bottom-right corner. Pointer conversion samples the same
engine texel that the composite uses at the device pixel centre, then clamps to the panel.
The inverse maps the centre of an engine texel back into its device footprint. Device extents
round to the nearest integer: truncation can omit the first source texel at a fractional scale.
Pixel centres within the rounded bounds still sample inside the source rectangle.

The enabled scale is published separately from the dock's, because a modal hides the cards while
the dock may remain drawn underneath it. At the game-thread flip, `OP_FLIP.dy` captures the card
scale; `PK_FRAME.h` carries it with that frame's drawing operations. The consumer resolves the
rectangle from this marker, not from a newer live input word. A drain consumes through its
initially captured queue head, bounded by the 65,535 published slots, so it cannot stop between
a window's drawing and its marker. If capture cannot append the terminating flip marker, it
discards that window before publication and requests a reseed for the next complete window. `uCards`, `uCardsMap` and `uCardsS`
occupy std140 offsets 128, 144 and 160 in the 176-byte composite uniform block. At 1× the card
pixels already lie at their device positions and the magnification branch is inert.

## Verification

On the reference setup, a four-seat recording showed compact cards and an accessible dock at
640×480, normal cards with resource rates at 1280×720, and correctly placed compact cards at
1280×720 with HUD 150%. Device-space clicks selected Sight and Full map at 150%. A paused switch
between seats and Full map kept GameTime at 476; Full map kept a separately verified camera at
(1800, 1900). These checks precede the final refinements to commander presence and frame ordering.

A separate **synthetic display fixture**, made by adding five named empty seats to that recording's
header while retaining its recorded player images, exercised nine rows at 640×480. It passed the
recording stream check, scrolled to the last row and routed that row's Sight to seat 8. The wheel
produced no zoom gesture. A press begun on a card, followed by a confirmed ARMOPT dialog and a
release, left the full-map viewer unchanged. The runtime DLL matched the build used for these
checks. This fixture verifies list layout and input routing, not multiplayer simulation.

A fresh `battle-los` recording passed the replay harness's live, puppet, round-trip and fresh-seek
legs, including the byte comparisons and radar picture. With the final commander labels and
fractional-bound fix, a separate playback opened in perspective 0 and sought to iteration 331.
At paused GameTime 330, Full map followed by Sight 0 recreated six death-vision records. Its frame
dump compared equal to the frame dump before switching, including player sight counters. The
same comparison at GameTime 1900 (iteration 1901, the fixture's LOS-mode change) also compared
equal, with two death-vision records recreated. These are perspective-preservation checks; they
do not claim that every possible sequence of seat switches was tested.

Full map and Sight 0 also both succeeded at GameTime zero after seeking to iteration 1. Seeking
again from zero requires a playback step, as the existing seek gate specifies. After the recording
ended at iteration 3990, both controls still worked while the host clock continued beyond EOF;
the cards retained the final recorded resources, three defeated seats and one surviving unit
without a commander. The 1280×720 normal-card status, rates and disabled-camera explanation were
visually inspected. The runtime DLL was checked against the build used for those checks.

A native test compiled the actual pure region maps and pointer conversions. Across six surface/
scale combinations from 640×480 at 1× to 3840×2160 at 4.5×, every card source texel round-tripped
and every device pixel mapped inside the source. A further sweep of every Q8 scale from 256 to
2048 checked each valid 3840×2160 map's four corners and shader pixel-centre bounds. Together
these made 2,487,360 round-trip/containment checks. This verifies the coordinate arithmetic, not
rendering at every size.

The presented cards and device-space Sight/Full map controls were also checked at 1920×1080
with Auto (225%) and 1024×768 at 125%. A wheel in the world gap still produced the zoom gesture;
Config replaced the dock and cards. At 1024×768, Leave from a direct command-line playback
returned to the main menu; opening REPLAYS and Watch again restored the cards. A Sight press
released outside its card left the viewed seat unchanged. These checks supplement the confirmed
modal-cancellation check above; they do not separately repeat the dock's OS focus-loss tests.

The harness's W leg also clicks the first card's Sight and the Full map footer while paused,
checking the viewed seat, clock and camera. It runs in the fresh worker's unscaled HUD, on both
stock-limits and raised-limits Watch fixtures.

The full landing replay gate passed on `25fce201`: 18 fixtures, with all six changed replay
functions reached in the required builds. The stock-limits Watch leg switched Sight 0 → Full map
4 at GameTime 450; the raised-limits Watch leg switched Sight 0 → Full map 2 at GameTime 600.
Both kept the paused clock and camera unchanged. Every fixture's recorded/playback comparisons,
round trips, fresh seeks and applicable takeover/fork checks passed. The gate attached a
`replay-roundtrip:` note with `reached=6/6 result=pass` to that commit.

Independent review and local landing remain pending. The dock and cards currently select retail art even on Escalation; mod-specific dock
selection is a pre-existing open item in the S5 design.


### Review corrections and final UI check

The synchronization review found two ways a draw could lose its matching scale marker:
the consumer's 20,000-operation cutoff and a full capture array omitting the final flip.
The consumer now drains through the captured publication boundary, and the producer rejects
captures without their terminating marker before publication. The next complete window reseeds
producer caches and consumer surfaces. A focused independent review verified cleanup, retirement
and the pending reset across disabled or stalled publication.

Native control-flow tests extracted the queue traversal and marker assignment from the source,
with raster branches stubbed: six cases covered batches of 20,010 and 65,534 operations, ring
and unsigned-counter wrap, and a concurrently published batch left for the next drain. The old
cutoff failed the same fixture. Extracted capture branches accepted the final available slot and
rejected a full array or missing surface before publication. These tests do not execute the full
raster or snapshot implementation; the latter's cleanup was verified in the focused review.

The rebuilt DLL then ran a four-seat replay at 1280×720 with repeated `reseedstress=120` resets,
first at stock HUD and then at 150%. Presented pixels at 150% were inspected; device clicks
selected seat 0 and full-map viewer 4. Config replaced the cards, closing it restored the dock
and cards, and another Sight click selected seat 0. Both standard and stock-limit builds passed.
The replay gate remains valid because these corrections change presentation files only.
