# Demo recorder 4c: every player's camera, cursor, clicks and selection (the plan)

## Status

Planned 2026-10-06; the four landings are built, their verification runs passed on 2026-10-06,
and they land with the multiplayer slice's stages 1–5: 4c-1 (the skirmish capture), 4c-2 (Follow, Jump and Free cam on
a skirmish playback, [as built](#4c-2-as-built)), 4c-3 (the overlays and their switches,
[as built](#4c-3-as-built)) and 4c-4 (multiplayer, contribution kinds 13-15,
[as built](#4c-4-as-built)). This is stage 4c of the
[multiplayer slice](demo-recorder-multiplayer-slice.md#stage-4-perspectives-and-communication)
and landing 3 of [S5](demo-recorder-s5-ui.md#landings) ("the streams and the cards"). It records
each human seat's camera, cursor, clicks and selection: the local human in a skirmish recording
(owner decision 2026-10-06), and each Impure peer's human in a multiplayer one, as contributions.
It also plays them back as Follow, Jump and the free camera, on the replay HUD's rows
([The replay HUD in game](../hud-in-game.md), *Input*), with three switchable overlays whose
switches are on the HUD's foot plate. They were built first on the player cards and the watching
dock's Overlays menu, which the replay HUD replaced and which are deleted. AI seats have no camera
or cursor and record none of these.

The product contract is [demo-recorder.md](demo-recorder.md#player-perspectives-and-overlays):
free camera, an optional Follow that reproduces the recorded position and zoom and ends on a
manual pan or zoom, and switchable overlays for every player's cursor, clicks and selection.
Queued orders are already shown by the engine's own markers from the recorded queues. Full
interface capture and drag rectangles are not part of it.

Evidence tags: **DISASSEMBLED** (`objdump -d -M intel` of `pristine/TotalA.exe.pristine`, this
session unless a note is cited), **MEASURED** (with the numbers), **[INFERRED]**.

## What the engine gives us

| # | Fact | Evidence |
|---|---|---|
| E1 | **One mouse record per frame, copied on the game thread.** IdleTick `0x499890` peeks the event ring (`0x4C2DE0` at `0x499982`), runs the GUI dispatch `0x4A9FD0` (`0x499992`) and peeks again (`0x49999C`). If the GUI left the record, it pops it into **`main+0x2C76`** (`0x4C2D60` at `0x4999C4`). Otherwise it copies one of the two peeked records there (`rep movs` at `0x499A04`); a button release the GUI took is still handed on (`0x4999CB..0x4999D7`). It then calls the input mode's handler `call [main+0x391F5]` once (`0x499A1C`). | DISASSEMBLED `0x499890..0x499A2A` |
| E2 | **The mode-6 handler `0x499200` computes everything a click acts on, then runs the frame.** It copies `main+0x2C76` to its stack (`0x499210..0x49921A`). Then `0x498DA0` (`0x499221`) fills the world point `main+0x2CAA`, the region bits `main+0x2CC6`, the cell `main+0x2C8E` and the hovered feature `main+0x2CBC`. The cursor index `main+0x2CBE` follows (`0x49925D`, `0x4992AD`), then the button tail. **Every path reaches `0x4995B8 call 0x496790`** (GameFrame_InGame): all 41 branches in `[0x499200, 0x4995B8)` stay inside the range or go to `0x4995B8`, and there is no `ret`. | DISASSEMBLED, branch-target census |
| E3 | **The frame's order** in GameFrame `0x496790`: the options-stack bit `main+0x37EBE` bit 0 skips to `0x49697B` (`0x49691E`), and the pause bit `main+0x38A51` bit 0 skips the ticks (`0x496926`). The tick loop `0x495490` runs at `0x49693E`, with B2 at `0x4954ED` and the camera stepper `0x41CA10` at `0x495599` inside it. Then come `0x495E90` (`0x496971`), the scroll poll `0x41CE90` (`0x496976`), the HotUnits cull (`0x49697B`) and DrawGameScreen (`0x4969CD`). The replay's frame visit sits just before `0x4969CD`, the zoom apply at the draw's top and the packet fill after it (`0x4969D2`). **So in one frame: E1, then E2's world point and clicks, then B2, then the scroll poll, then the frame visit, then the apply, then the draw.** | DISASSEMBLED `0x496918..0x4969D2`; the frame visit `tagpu_replay.c` THE FRAME VISIT; the apply `tagpu_zoom.h` |
| E4 | **The world point `main+0x2CAA`** is three 16.16 dwords: x at `+0x2CAA`, height at `+0x2CAE` and z at `+0x2CB2`. `GetTPosition 0x484B50` writes it, called at `0x498EF9` with `main+0x2CAA` (`0x498EF1`). The tail shifts `+0` and `+8` right by `0x14` into the cell pair (`0x498F0A..0x498F2E`), and DrawGameScreen reads the integer halves `+0x2CAC/+0x2CB0/+0x2CB4` as words (`0x468DC4..0x468DD2`). The only other direct writer is the console command `0x416860`, which adds to x and z (`0x4168A4`, `0x4168BA`). In Impure, `0x499221` is vpwide's `vpw_mouse_world`, which hands the engine its unzoomed point, and `0x498EF9` is the zoom's GetTPosition guard. **So the point is the world point under the true pointer at any zoom, clamped inside the map.** | DISASSEMBLED, a displacement scan of the image. Sites that pass the address as an argument (`0x41781E`, `0x417873`, `0x41789F`, `0x4179ED`, `0x417A44`, `0x4196A8`, `0x4197F7`, `0x48D095`, `0x48D3D4`, `0x499073`, `0x4991E4`) were not followed |
| E5 | **Region bits `main+0x2CC6`:** bit 0 is the minimap (`0x498DE4`, cleared `0x498E8E`), bit 1 the world viewport (`0x498EBC`), and bit 2 either of them (`0x498EE6`). | DISASSEMBLED |
| E6 | **The two click actions each have exactly one call site in the image.** The left click's action `0x498F70` is called only at **`0x4995B3`** (`e8 b8 f9 ff ff`). It is reached from a command-mode left press (`0x4993C5..0x4993D0`), a short, still left release (fewer than 25 clock units and 32 px; `0x499435..0x499441`, then `0x4995AE`), and an Interface-Type-0 minimap press (`0x4995AA`). The right button's press `0x499100` is called only at **`0x4993AC`** (`e8 4f fd ff ff`), on `WM_RBUTTONDOWN` (`0x49939F`) unless `[main+0x2CDF]` (`0x499387`). Both take the stack copy of the record, whose wParam is at `+8`. When they run, E2 has already stored this message's world point, region and cursor index. | DISASSEMBLED, a full-image scan of `call`/`jmp` to both entries. Impure's `order_check` calls `0x499100` directly from the DLL, which is not a click |
| E7 | **The camera.** The eye is `main+0x1431F/+0x14323`, the scroll target `main+0x14327/+0x1432B`, and the 1x view `W`/`H` `main+0x37E37/+0x37E3B`. Every camera move a player makes writes the target: the nine eye writers copy the eye into it, the smooth centre-ons store it, and the stepper's follow source writes it. Only the stepper's ease and **the shake `0x41C6F0`** move the eye alone. The shake adds an offset at `0x41C784`/`0x41C797` while `main+0x1434E` bit 0 is set and `main+0x14333 > 0`, with random values from `0x4E4870` [INFERRED: the C runtime's `rand`]. | The [camera module](../engine/input.md#the-camera-module-mapped-by-us) (DISASSEMBLED 2026-09-23/24); `0x41C6F0` re-read this session |
| E8 | **No read in this design crosses threads.** The mouse thread `0x4C2990` writes only the graphics object `0x51F320` (`+0x196` = `0x51F4B6`, `+0x1B6` = `0x51F4D6`; `replay-globals.tsv` rows `cursorpos` and `cursordrawn`). Every field in E1-E7 is written by game-thread code. | `replay-globals.tsv`; E1-E4 |
| E9 | **The selection.** Unit `+0x110` bit 4 (`0x10`) is the selection and bit 28 (`0x10000000`) the live bit (`0x48CD08`, `0x48CD32`). The controlled player's block is `[rec+0x67, rec+0x6B]`, inclusive pointers, with `rec = main+0x1B63 + 0x14B*main[0x2A42]`. Orders and placements go to selected units of that block (`0x48CF30`, `0x419670`). The destructor clears bits 4 and 5 (`0x486DE8`). The recording does not carry bit 4: `replay-layout.tsv` row `selected` is `drop`. | [engine map](../engine/input.md#the-in-game-mouse-buttons-what-a-click-actually-does-mapped-by-us); `replay-layout.tsv` |
| E10 | **Births.** `src/tagpu_patches.c:s_hitStamp` is a SIM_STATE column of one u32 per slot; bit 31 (`HIT_LB`) marks a lower bound. `mp_births()` finds it through the registry. A skirmish recording carries it as a declared table. **On a skirmish puppet host the engine-side column is host-own**, so the playback reads the recorded column. In a multiplayer playback the composed lifecycle's `ls.birth` is the reference, as for order lists (`mp_authority`). | `tagpu_patches.c:6518`; `tagpu_replay.c` THE SEEK (R-steps, "B4's per-slot columns") and `mp_authority` |
| E11 | **The input mode `main+0x391F1`** is stored by `0x490B30` (`0x490B3D`). It is also stored directly with 2-5 at `0x496AF0`, `0x496B84`, `0x496BE6`, `0x496C2E`, `0x496C95` and `0x496D65`, and with register values at `0x49160B`, `0x492972` and `0x492A73`. Mode 6 installs `0x499200`. **Not established:** which of modes 2-5 occur in play, and whether their handlers refresh E4. | DISASSEMBLED, the stores only |
| E12 | **The contribution channel.** It sends `DPSEND_GUARANTEED \| DPSEND_ASYNC` to all players (`tagpu_recmp_net.c:344`) through a 256-slot transmit queue whose overflow fails the channel (`tagpu_recmp.c:467`). `send_from` accepts only the current capture tick. `RecMpHeader` is 64 bytes, and a 16-byte body made one 80-byte DATA in one 94-byte UDP payload (MEASURED 2026-10-05, [datagram probe](demo-recorder-multiplayer-slice.md#native-contribution-datagram-probe)). | Code; slice note |

## Decisions

| Question | Decision | Why |
|---|---|---|
| The camera position | **The scroll target plus half the view**: `cam = target + (W/2, H/2)`, which is the world point at the view's centre and does not change with zoom | Every player camera move writes the target (E7). The target carries no shake noise, which a 10 Hz sample of the eye would turn into a slow wobble. Smooth centre-ons glide anyway through interpolation |
| The zoom | **Impure's applied level** (`tagpu_zoom_applied`), the level the latest command record carried, as Q16 | It is the level the render thread drew, read on the game thread |
| The cursor | **The engine's world point `main+0x2CAA` and region bits at B2**, not `g_ddraw.cursor` or `0x51F4B6` | It is the game thread's own value from this frame (E1-E4, E8), already clamped to the map. Recomputing it would read the message thread's word |
| A click | **The two engine click actions**, recorded at their only call sites (E6): the world point, the button, the modifiers, and the dispatch's inputs (the order byte, the cursor index this message produced, the region and the Interface Type) | One redirect per action covers command modes, contextual clicks, placements and minimap clicks, in exactly the engine's sense of a click. The record holds no order content: the order queues are already recorded and drawn by the engine's markers, so nothing is recorded twice |
| A selection | **The full set of `(slot, birth)` for the controlled block's live units with bit 4, emitted when it changes. A death is not a change**: compare the new set with the last record's entries that are still alive with the same birth | Deaths in a battle would otherwise send the set every tick. In a multiplayer game that is about 1.2 KB per tick per peer for 200 selected units. The display rule (below) hides dead entries anyway |
| Sampling | **Camera and cursor: one combined sample, at most one per `every` ticks, only when it differs from the last one sent** (skirmish `every = 3`, 10 Hz; multiplayer: owner Q3). **Selection: checked every B2. Click: when it happens** (at most one per frame, E1) | Contract: continuous data only when it changes, a few times a second; discrete data reliable and in order |
| Lost samples | **Multiplayer only, camera and cursor only:** the producer skips a sample when the transmit queue has fewer than 64 free slots, and the next eligible tick sends it if it still differs | The contract allows a lost sample; a full queue fails every contribution (E12). The check is safe because the game thread is the queue's only producer: free slots can only grow between the check and the publish |
| Interpolation | **Camera and cursor play `every` ticks behind the world**, interpolated between the two samples around that time with integer arithmetic. **Clicks and selections play at their own tick** | The agreed method is interpolation from earlier and later samples ([How it works](demo-recorder.md#how-it-works)). Since samples are at least `every` apart, a delay of `every` always has both in hand without reading ahead. Discrete events stay with the units they refer to |
| Format | **Shared payloads** in a new `inc/tagpu_recpov.h`. **Skirmish:** `RK_POV` 52, `RK_CLICK` 53, `RK_SELECT` 54, the snapshot-only `RK_POV_AT` 55, `REC_FORMAT` 7, and readers accept 5..7. **Multiplayer:** `RECMP_POV` 13, `RECMP_CLICK` 14, `RECMP_SELECT` 15. A network-mode recorder writes none of the RK kinds | One payload and one consumer per kind. A multiplayer file has one source for perspectives, the contribution ledger, as `ev_put` already does for owner-only roots |
| Follow's mechanism | **The apply writes the eye and target every draw from the interpolated sample.** Follow ends when the target at the apply's entry differs from Follow's last write, or when the render thread reports a wheel notch for Follow's epoch | The apply runs after every engine camera writer of the frame (E3), and every player camera move writes the target (E7). That makes "someone else moved the camera" an observed fact, not a guess |
| Overlays' renderer | **Vulkan:** cursors and click rings in the marker pass with fog off, and selections as the native pass's selection rectangle in the player's colour. **GDI: none** (owner Q2) | The marker pass already draws world-anchored geometry through the zoom transform. The selection rectangle is the engine's own look for "selected" |
| Simulation-state class | **None of the new statics is simulation state.** All of them live in `presentation` files (`tagpu_recorder.c`, `tagpu_replay.c`, `tagpu_zoom.c`, `tagpu_native.c`, `tagpu_mark.c`) or in new engine-free files (`tagpu_persp.c`, `tagpu_recmp_pov.c`), so `tagpu_simreg.h` declares nothing. The simulation reads none of them. The camera rows stay `leave` and `selected` stays `drop` | No loosening: no exclusion, no `presentation` change to a sim file, no tolerance |

## The records (`inc/tagpu_recpov.h`, new; packed, little-endian)

```c
#define REC_POV_EVERY    3        /* skirmish: ticks between samples, and the playback's delay */
#define RECMP_POV_EVERY  6        /* multiplayer: owner Q3 (6 = 5 Hz, 3 = 10 Hz)               */
#define RECMP_POV_ROOM   64       /* free transmit slots a camera/cursor sample requires       */
#define REC_CLICK_SHOW   15       /* ticks a click ring lasts                                   */
#define REC_SEL_MAX      1500     /* entries a selection may hold: >= TAGPU_LIM_UNITS of every
                                     build (static assert), the bound of one seat's block       */
enum { REC_POV_WORLD = 1, REC_POV_MINIMAP = 2 };
enum { REC_SEL_BEGIN = 1, REC_SEL_END = 2 };
#pragma pack(push, 1)
typedef struct RecPov {           /* 32 bytes */
    uint8_t  player;              /* seat row; 0xFF in a contribution, whose origin names it */
    uint8_t  every;               /* the sampler's spacing: the playback's delay             */
    uint8_t  flags;               /* REC_POV_*: main+0x2CC6 bit 1 / bit 0                     */
    uint8_t  zero;
    uint16_t view_w, view_h;      /* main+0x37E37 / +0x37E3B                                  */
    uint32_t zoom;                /* applied level x 65536, 16384..524288                     */
    int32_t  cam_x, cam_y;        /* target main+0x14327/+0x1432B + view/2, world px          */
    int32_t  cur_x, cur_h, cur_z; /* main+0x2CAC/+0x2CB0/+0x2CB4 sign-extended (E4)           */
} RecPov;
typedef struct RecClick {         /* 20 bytes */
    uint8_t  player;              /* as RecPov                                               */
    uint8_t  button;              /* 0 the left action 0x498F70, 1 the right press 0x499100   */
    uint8_t  order;               /* main+0x2CC3                                             */
    uint8_t  cursor;              /* main+0x2CBE                                             */
    uint8_t  region;              /* main+0x2CC6 & 3                                         */
    uint8_t  itype;               /* main+0x37EFA                                            */
    uint16_t keys;                /* record wParam & (MK_SHIFT | MK_CONTROL)                 */
    int32_t  x, h, z;             /* as RecPov's cursor                                      */
} RecClick;
typedef struct RecSel {           /* 8 bytes, then `count` entries */
    uint8_t  player, flags;       /* a skirmish record is BEGIN|END, first 0, count = total  */
    uint16_t total, first, count;
} RecSel;
typedef struct RecSelEntry { uint16_t slot; uint32_t birth; } RecSelEntry;   /* 6 bytes */
#pragma pack(pop)
```

A stream's `RK_POV` takes its tick from its iteration, which a snapshot cannot give the samples
taken before it. So a snapshot carries each seat's samples as **`RK_POV_AT` 55**: `u32` the
GameTime the sample was taken, then the `RecPov`, 36 bytes. A stream refuses `RK_POV_AT`, a
snapshot refuses `RK_POV` and `RK_CLICK`, and every kind keeps one length everywhere.

Validation, the same in the C readers, the history and both Python mirrors:
- **Exact lengths:** 32, 20, 36 for `RK_POV_AT`, and `8 + 6*count`.
- **`RecPov`:** `every` in 1..30, `flags` within the two bits, `zero` 0, `zoom` in range, `view_w`/`view_h` in 1..16384.
- **`RecClick`:** `button` 1 or less, `region` 3 or less, `itype` 1 or less.
- **`RecSel`:** `total` at most the seat's block (`units_each`) and REC_SEL_MAX, `first + count <= total`, slots strictly ascending and inside the seat's block, and a transaction contiguous on its stream that publishes only at END.
- **Coordinates are data:** never an index, bounded at use by the projection's clip.

**Sizes and budget.** In a skirmish file, an `RK_POV` is 36 bytes with its header, at most 360 B/s while
moving. A click is 24 bytes, a selection change `12 + 6n`. In a multiplayer game, each contribution
carries the 64-byte envelope. A sample is 64 + 32 + 14 (DirectPlay's prefix, E12) = **110 B per
peer link** before IP/UDP headers and DirectPlay's acknowledgements [INFERRED: one datagram per
remote peer for `DPID_ALLPLAYERS`]. A click is 98 B, a selection change `86 + 6n`. Take continuous
motion, one click and half a selection change of 30 units each second. That is **1.33 KB/s per
link at 10 Hz and 0.78 KB/s at 5 Hz**. In a ten-player game that makes **12 KB/s and 7 KB/s
outgoing per player**. This is 2-4 times the contract's estimate of 1.5-3 KB/s, because that
estimate counted no envelope. The physical budget gate in the slice note is still open (owner Q3).

## Capture (`tagpu_recorder.c`, game thread)

**B2, tick T, for each seat whose controller `+0x73` is 1** (the skirmish's `main+0x2A42`, or each
local multiplayer actor):

1. **The camera and cursor.** Skip the tick if the last sample was sent at most `every - 1` ticks
   ago. Otherwise build a `RecPov` and send it only if it differs (memcmp) from the last one sent;
   then remember it and T. A multiplayer sample is also skipped while
   `tagpu_recmp_room(c) < RECMP_POV_ROOM`, leaving the last-sent copy unchanged.
2. **The selection.** Walk the controlled block. The bound is that of `tagpu_patches.c`'s
   `player_units`: first and last inside `[main+0x14357, main+0x1435B]`, aligned to `0x118`, and
   at most `REC_SEL_MAX` slots, refusing a larger block once with a log line. Build the ascending
   list of live (bit 28) selected (bit 4) units with `birth[slot]` from `mp_births(L)`, under the
   same refusal when the column is missing. Filter the last record's entries to those still live
   with the same birth. Send the full set when the two lists differ. A skirmish set is one
   `RK_SELECT` (`9008` bytes at most, under the `u16` length). A multiplayer set is
   `RECMP_SELECT` chunks of at most `(RECMP_BODY_MAX - 8) / 6 = 670` entries, BEGIN to END.

**Click hooks:** call-site redirects at `0x4995B3` (left) and `0x4993AC` (right), byte-matched at
attach and installed with the recorder's other sites whether or not a recording runs, because
multiplayer contributions are mandatory. Each is `stdcall(rec*)`: it reads E6's fields and records
them, then calls the original with the same argument.

- **Skirmish:** `ring_put(RK_CLICK)` through `ev_rec()`'s thread check. It lands in iteration T's
  records, T being the last B2, and a full ring ends the recording as `ev_put` does.
- **Network mode:** `tagpu_recmp_send_from(RECMP_CLICK, tick = the channel's capture tick)`. It is
  published directly, because `send_from` accepts the current capture tick between two B2s
  (E12), and only after the channel's first advance.

Comments the capture must carry:
- **The ORDERING of E1-E3:** B2 reads this frame's world point and the camera as the last apply,
  this frame's 0x499200 and the stepper left it.
- **The single-thread argument of E8**, naming the mouse thread's two words.
- **The BOUND of the block walk.**
- **At the hooks:** the one-call-site fact, with the scan as its evidence, and that 0x498DA0 and
  0x4992AD have already run for this message.

## Playback

**Perspective state** (engine-free `src/tagpu_persp.c/.h`; its inputs are copied values):
- **Per seat:** the last two samples with their ticks, and the current selection (ascending
  `(slot, birth)`, capacity `units_each`).
- **Clicks:** a ring of the 64 most recent, each with its tick.
- **Skirmish:** `RecWorld` holds the samples with their ticks and the selection. `recr_tick` and
  `recr_seek` maintain them, and the writer's snapshot writes each seat's two samples as
  `RK_POV_AT` in order, then its `RK_SELECT`, right after the `RK_VIEWER`. The iteration's clicks
  are a per-iteration list in `RecWorld`, not in its events: a click is taken outside the tick
  loop, so it follows the step's events, outside every bracket. A list grows as the events' does,
  so clicks made while the game is paused need no cap of their own; a full ring ends the
  recording as it does for an event. An Interface Type above 1 records no click, counted and
  logged once.
- **Multiplayer:** the history's applied caches (`recr_mp_pov`, `recr_mp_select`) at the reader's
  application tick, rebuilt at a seek as the explored map is. Clicks come from the step's held
  envelopes (`mp_envelope`) at their application tick, as chat does. The step's root-batch split
  ignores kinds 13-15.

**The perspective clock** at the frame visit, for the displayed tick `n` (skirmish `W->tick`,
multiplayer the composed application tick):
- **The sample:** with `b` the latest sample and `a` the one before, let `t = n - b.every`. If
  `b.tick <= t`, use `b`.
- **Interpolation:** if instead `a.tick <= t < b.tick` and `b.tick - a.tick <= 2*every`,
  interpolate every field as `a + trunc((b-a)*(t-a.tick)/(b.tick-a.tick))` in 64-bit, truncating
  toward zero, and mirror that in Python. Interpolate the cursor only when both samples have
  `REC_POV_WORLD`. Otherwise use `a`.
- **Why two samples suffice:** samples are at least `every` apart, so `a.tick <= t` whenever
  `b.tick > t`. In multiplayer, application ticks can bunch, so the rule falls back to `a`.

**Follow, Jump and Free cam** (`tagpu_replay.c` with `tagpu_persp.c`; `tagpu_zoom.c`):
- **The request.** A row's Follow on the replay HUD (`follow:i`) or `tacli replay follow <seat>`
  starts epoch `E` (a game-thread counter). It is refused while the seat has no sample.
- **The frame visit** computes, after any seek, the viewer's eye `cam - (W_v/2, H_v/2)` from
  `main+0x37E37/+0x37E3B` and the level. Owner Q1 recommends fitting the recorded view:
  `z_v = clamp(z_r * min(W_v/W_r, H_v/H_r), 0.25, 8)`, in Q16 integer arithmetic. It then calls
  the new `tagpu_zoom_follow(ta, eyeX, eyeY, levelQ16, E, once)`. That function hands the
  position to the next apply, and on GDI, where no apply runs, writes it at once with
  `tagpu_zoom_place_eye`'s bookkeeping.
- **The apply,** in order:
  1. If E is active and this is not its first write, Follow ends when the target at entry differs
     from Follow's last post-clamp write ("the camera moved"), or when `c->follow_broken == E`
     ("zoomed").
  2. The first write of an epoch first releases the engine's own follow (`release_follow`: the
     countdown, the unit and the object), so an earlier Ctrl+C cannot end it at once.
  3. The anchor delta (unchanged).
  4. Follow's write of the eye and the target, unless the hold is on, which wins.
  5. The hold, the range clamp and the bookkeeping, unchanged.
  6. Remember the post-clamp target.
  The draw publishes `follow_epoch = E` and `follow_level` in the packet.
- **The render thread** (`tagpu_zoom_read_lever`) treats the file as the first lever, Follow as
  the second and the wheel as the third. The follow lever is in force for a packet whose epoch is
  nonzero, not marked broken, and whose level lies in `[ZOOM_MIN, ZOOM_MAX]`:
  - **Notches queued:** set `s_followBroken = epoch` and start the wheel from the follow level
    without discarding the notches. `wheel_level` and `anchor_step` then run as for any notch.
  - **Otherwise:** pin the wheel to the follow level, apply no anchor, and set
    `s_followSeen = epoch`.
  - **Every command record** carries `follow_broken` and `follow_seen`.
- **When Follow ends,** for any reason, the wheel keeps the last follow level, because it was
  pinned there.
- **Jump** (`jump:i`) is a `once` epoch. The apply keeps writing the camera until
  `c->follow_seen == E`, so the render thread has drawn its level at least once, then ends the
  epoch with no "broken" report.
- **Free cam** — the pressed Follow clicked again, or `tacli replay follow <i> free` — ends the
  epoch.
- **The HUD and the log.** The replay polls `tagpu_zoom_follow_state(E)` at the frame visit. The
  HUD draws Follow pressed on the followed seat's row (`tagpu_replay_followed`), and none under
  the free camera. One log line records each start and end, with its reason.

**Overlays** (built at the frame visit after `recorded_put`; copied by value into the packet at
`0x4969D2`; no pointer crosses):

| Overlay | Data | Drawn by |
|---|---|---|
| Cursors | A seat whose interpolated sample has `REC_POV_WORLD` gives `PK_OVERLAY{kind 0, player, colour, x, h, z}` | `tagpu_mark.c`: a filled arrow of constant screen size at the projected point (`x - eyeX + vpL`, `z - h/2 - eyeY + vpT`, offsets divided by the frame's zoom) with a one-pixel dark outline, in its own draw list entry with fog off, after the fog overlay |
| Clicks | A click with `0 <= n - tick < REC_CLICK_SHOW` gives `PK_OVERLAY{kind 1, …, age}` | `tagpu_mark.c`: a 16-segment ring in the player's colour, its screen radius shrinking with age |
| Selections | A slot in the seat's selection whose displayed unit is live **with the entry's birth** (E10: skirmish from the recorded births column in `RecWorld`, found by its RT_TABLE name; multiplayer from `ls.birth`) gets bit `p` of `PK_UNIT.rsel` | `tagpu_native.c`: `selbox_emit` for `pu->rsel` in `pk->ov_col[lowest bit]`, depth-tested like the engine's own rectangle |

Further rules:
- **Colours** come from `tagpu_replay.c`'s `seat_colours` (`logo_colour` of each seat's logo),
  computed once a playing and published in `ov_col[10]`.
- **Switches:** three game-thread bits (Cursors, Clicks, Selections), default on, reset at each
  playback, not persisted, beside the chat's: the replay HUD's foot plate (owner Q4 put them with
  the chat's).
  A switched-off overlay produces no items, so the render side needs no switch.
- **Players:** every recorded player is shown at once, whatever Sight shows (owner Q5).
- **Packet:** `PK_UNIT` replaces `pad_[3]` with `uint8_t pad0; uint16_t rsel;` (offsets 85 and 86,
  size still 104, asserted). New fields `n_overlay/off_overlay` (at most 128 items of 16 B),
  `ov_col[10]`, `follow_epoch` and `follow_level`. `TAGPU_CMD` gains `follow_broken` and
  `follow_seen`.

**At a seek** (and at a timeline switch): the state comes back from the reader (snapshot plus
stream) or from the history, the click ring is emptied (no click from before T, as for chat), and
Follow stays on and snaps to the new time. The seek leaves the camera alone, so the
target-compare does not fire. **Takeover:** Follow and the overlays end. **After the end of the
file:** the last state holds and clicks expire. **Level end:** the zoom's epoch bump ends
everything.

**The HUD's rows:** Jump and Follow act for a seat with a sample; on one without, they show the
toast "Camera was not recorded". The three switches are on the foot plate with the chat's.
**tacli** triggers: `replay follow <seat>|free`, `replay jump <seat>`, `replay overlay <name>
on|off`, and `replay pov`, which logs the follow epoch, the computed eye and level, the item
count and the number of `rsel` units.

### 4c-2 as built

Built 2026-10-06; its V2 run passed the same day (*Status*). The code: `src/tagpu_persp.c` (the clock and the fit),
the follow lever and the follow's apply in `src/tagpu_zoom.c`, the frame visit's part
(`follow_visit`) and the requests in `src/tagpu_replay.c`, the controls (then the cards, now the
replay HUD's rows, `src/tagpu_hudinput.c`), and `tacli replay follow|jump|pov`. What it settled that the plan left open:

- **The fit rounds down.** Each of `z_r * W_v / W_r` and `z_r * H_v / H_r` is truncated in 64-bit
  integers before the minimum and the clamp, so the viewer's view/z_v is never smaller than the
  recorder's view/z_r unless the 0.25 floor cuts it. The eye is `cam - W_v/2` with the half
  rounded down, saturated to int32.
- **Interpolation starts from `a`.** The fields the clock does not interpolate (`player`,
  `every`, `flags`) and, unless both samples are `REC_POV_WORLD`, the cursor are `a`'s. With one
  sample, a latest that is too new answers with that sample (`PERSP_EARLIER`).
- **The mirror is checked against the C, not only against the plan.**
  `tools/replay/persp_test.py` builds `tools/replay/persp_test.c` with `tagpu_persp.c` with the
  host's C compiler, runs its rule checks, then 200 000 random cases (int32-wide coordinates
  included) against `tarec.py`'s `persp_sample` and `persp_follow`: 0 differ. A mirror that
  floors instead of truncating fails it (821 of 20 000).
- **The displayed tick is the reader's, `W->tick`**, which is GameTime + 1 while playing (the
  frame visit shows the iteration after the step). `tacli replay pov` logs it as `tick n`, and
  `tarec.py pov FILE --eye n --viewer W,H` takes that n. Its eye is the one before the camera's
  range clamp; the apply clamps into the range in force, so near a map edge the peeked eye is the
  clamped one.
- **Epochs are the replay's**, a counter over the process that is never reset, so an epoch is
  never reused across levels; `tagpu_zoom` ignores an epoch that is not the one in force and not
  later than every one it was handed. The render thread compares the packet's and the record's
  epochs with the one in force by equality only.
- **A Jump is `seen` only when its frame is drawn at the follow level**; the fog bound can draw a
  frame at the last frame's level (`predict`), and that frame does not count. Where the level
  lever is not the follow's — `tagpu_zoom.txt` in force, or the 1.0 pin without the mouse-world
  repair — the render thread reports the epoch seen at once, so a Jump ends there with the file's
  level, which outranks it.
- **A scenario camera handed to the apply ends a Follow** as "camera moved": it is a camera move
  of someone else's that the target comparison cannot see, because it is written inside the
  apply.
- **No apply (GDI, the packet publisher count-only):** `tagpu_zoom_follow` writes the eye and the
  target at the frame visit, before the draw call, with what `tagpu_zoom_place_eye`'s own write
  owes the engine; the same target comparison ends a Follow there, and a Jump ends with its write.
- **A seek before the seat's first sample** leaves it none: the epoch stays and the camera stays
  where the last write put it.
- **The controls**: Jump and Follow act for a seat whose `RecWorld` holds a sample
  (`TAGPU_REPLAY_PLAYER.pov`, the packet's `pov`); Follow is drawn pressed on the followed seat
  and on none otherwise, a Jump included (`tagpu_replay_followed` is -1 under a Jump). On a seat
  without a sample they show the toast "Camera was not recorded".
- **Log lines**: `replay: <name>: follow seat <S>, epoch <E>` / `jump to seat <S>, epoch <E>`;
  `follow ended: camera moved|zoomed|free cam|another follow|a jump|the playing ended|the level
  ended` (`jump ended: drawn` for a completed Jump); `<follow|jump> <S> refused: <why>`; and
  `pov at GameTime <G>: follow|jump epoch <E> seat <S>, tick <n>, sample <how>, view <W>x<H>,
  eye (<x>, <y>), level <L>/65536 (<z>)`.

Gaps 4c-2 leaves:

- **A multiplayer playback refused Follow and Jump** here; [4c-4](#4c-4-as-built) brings them.
- **An arrow key on a paused playback neither scrolls nor ends a Follow.** The engine's scroll
  does not run while the game is paused, and never did on a playback: MEASURED 2026-10-06 on the
  build before 4c-2 (eye x 992 before and after a 1-s right arrow, paused) as on this one; running,
  the same key scrolled (0 to 992) and, under Follow, logged "follow ended: camera moved".
- **A Vulkan frame that skips `tagpu_zoom_read_lever`** (the overlay off, the reclaim teardown's
  frames) reports nothing: a Jump made while no frame reads the lever is written until the camera
  is moved, as a Follow would be.

### 4c-3 as built

Built 2026-10-06; its V3 run passed the same day (*Status*). The code: the builder and the switches in
`src/tagpu_replay.c` (THE OVERLAYS), the copy into the packet in `src/tagpu_packet_pub.c`
(`fill_overlay` and the unit walk's `rsel`), the acquire's checks in `src/tagpu_packet.c`, the
cursor and ring in `src/tagpu_mark.c` (`gather_overlay`), the recorded selection's rectangle in
`src/tagpu_native.c`, the switches' buttons (then the dock's Overlays menu, now the replay HUD's
foot plate, `src/tagpu_hudinput.c`), and `tacli replay overlay`. What it settled that the plan
left open:

- **The buttons.** The switches were first a menu of four toggles opened by the dock's DKOVER
  and drawn by the cards' layer; with the dock and the cards that menu is deleted, and the four
  are the replay HUD's foot-plate buttons `cursors`, `clicks`, `selections` and `chat`, each
  drawn pressed while on ([The replay HUD in game](../hud-in-game.md), *Input*).
- **The switches** are `TAGPU_OV_*` (`tagpu_replay.h`); Chat's is the existing hide-chat bit.
  All four come back on when a playing's state is reset: at the next playing's Start
  (`replay_fill`) and at the level's end. The triggers `overlay <cursors|clicks|selections|chat>
  <on|off>` and `chat hide|show` set them, and so do the HUD's foot-plate buttons. Each logs `replay: overlay <name>
  on|off`, or `replay: chat hidden|shown` for the chat, which `recmp_play_test.py --hide-chat`
  waits for.
- **The clicks' clock.** The ring takes an iteration's clicks where `play()` applies its events
  (each iteration once, a late one included), tagged with the iteration's GameTime. A click is
  shown while `GameTime + 1 - tick < REC_CLICK_SHOW`. `GameTime + 1` is `W->tick` while the
  playing runs and keeps growing after the recording's end, where `W->tick` stops: so a click
  expires there, while the cursor and the selection, which read `W->tick`, hold. A seek or a
  takeover empties the ring before it reads anything. So a click just before T is gone after a
  seek to T, and T's own clicks come with T's step. After a step the first frame shows a click at
  age 1.
- **Cursors** are the perspective clock's sample at `W->tick`, the one Follow takes, for every
  header seat with a sample whose `REC_POV_WORLD` bit is set.
- **The selection rule** finds the births column by its RT_TABLE name
  `src/tagpu_patches.c:s_hitStamp` (stride 4) in the header and reads its row in the world's table
  image. An entry is drawn when the slot is below the world's slot count, the column's rows and
  `TAGPU_PK_MAX_UNITS`, its recorded image is live with bit 28, and the column holds the entry's
  birth. A recording without the column shows no selections. On the render side the viewer's own
  selection keeps the engine's colour and wins. Otherwise the rectangle takes
  `ov_col[lowest set bit of rsel & 0x3FF]`. It is not behind the engine's SelBoxes option (the
  overlay has its own switch).
- **Colours** are taken once a playing, at the first frame visit or the first HUD fill that asks,
  from each header seat's `logo` through `logo_colour` (`seat_colours`, `tagpu_replay.c`). A
  player with no seat, or a logo past the colour sequence's count, gets 59 (`OV_GREY`).
- **The drawing.**
  - **Cursor**: an arrow 12 x 18 screen px, its tip at the projected point. The fill is five flat
    triangles in the player's colour, with a one-line outline in the GUI's black (`gui_col[0]`)
    over it.
  - **Click**: 16 line segments in the player's colour, radius `(4 + 12 * (15 - age) / 15)` screen
    px.
  - **Projection**: `selbox_emit`'s `x - eyeX + vpL`, `z - floor(h / 2) - eyeY + vpT`, with the
    offsets times one screen pixel in the frame's units. The anchor is snapped to the device grid.
    An item more than 32 screen px outside the zoom's effective rect makes no vertex.
  - **Order**: three draw-list entries after the build cursor, all with the fog off -- the rings,
    then the arrows' fills, then their outlines. That is 10 of the hand-over's 16.
- **The packet.** `PK_OVERLAY` is `{kind, player, colour, age, x, h, z}`, 16 bytes; at most 128
  ride a packet, after the build orders in the cut order (`TAGPU_PK_TRUNC_OVERLAY` when they do
  not fit). The acquire refuses a table past the cap or an item whose kind is above 1 or whose
  player is 10 or more. `PK_UNIT.pad0` is at 85 and `rsel` at 86, both asserted with the 104-byte
  size. The header gains `n_overlay`, `off_overlay`, `ov_col[10]` and two pad bytes. Outside a
  playback, all of it is zero.
- **The ordering**: the replay builds the items and the bits at its frame visit, before the draw
  call `0x4969CD`, and the publisher copies them by value in that draw's `after` at `0x4969D2`. Both
  run on the game thread, so a packet carries the overlays of its own frame. The render thread
  reads the copy only.
- **`replay pov`** appends `; overlays: <n> items, <m> selected units` to both of its lines. The
  overlays are built before Follow's part of the visit, so the counts are this visit's.
  `tacli replay pov` reports them as `overlay_items` and `selected_units`.

Gaps 4c-3 leaves:

- **A multiplayer playback showed no overlays** here; [4c-4](#4c-4-as-built) brings them.
- **GDI shows none** (owner Q2).

## Multiplayer (kinds 13-15)

- **Channel:** `send_from`'s accept list gains 13-15. A new `tagpu_recmp_room(c)` returns
  `MP_QUEUE_SLOTS - (published - read)` as the producer sees it.
- **Codec and caches:** `src/tagpu_recmp_pov.c/.h` validates the three payloads, assembles
  selection transactions (only END publishes, and a seal requires no partial transaction, as
  kind 11 does), and keeps fixed caches of `RECMP_ACTORS` × (two samples, one selection of
  `each` entries).
- **History:** `tagpu_recmp_history.c` gains source and applied caches, `semantic` branches and
  queries.
- **Reader and validator:** the reader adds `recr_mp_pov` and `recr_mp_select`. In
  `tools/replay/recmp_file.py`, `_semantic` mirrors the same checks and the applied state.
- **Capture:** `mp_capture` runs the B2 capture after the explored map for each local actor with
  controller 1. A slot outside the actor's roster block refuses the contribution.
- **Consumers:** the `tagpu_replay.c` consumers listed under Playback.
- **Build:** as with kind 11, every peer needs the same build.

### 4c-4 as built

Built 2026-10-06; its codec and file tests pass, and its two-peer run (V4) passed the same day
(*Gaps 4c-4 leaves*, below). The
code: the codec and caches in `src/tagpu_recmp_pov.c` (`inc/tagpu_recmp_pov.h`), the channel's
accept lists and `tagpu_recmp_room` in `src/tagpu_recmp.c`, `_record.c` and `_stream.c`, the
history's caches in `src/tagpu_recmp_history.c`, `recr_mp_pov` and `recr_mp_select` in
`src/tagpu_recreader.c`, the capture (`mp_pov_capture`) and the network click (`mp_click`) in
`src/tagpu_recorder.c`, the playback's composition (`mp_authority`, `mp_click`, `persp_world`,
`ov_born`) in `src/tagpu_replay.c`, and the Python mirror in `tools/replay/recmp_file.py`. What it
settled that the plan left open:

- **The payloads are tagpu_recpov.h's, with `player` 0xFF** (`RECMP_POV_PLAYER`) in every one:
  the envelope's origin names the actor, the codec refuses any other value, and the playback
  writes the seat the roster gives the origin.
- **One cache for the three kinds.** For each roster actor it keeps the last two samples, the
  last click (for inspection; the playback takes clicks from the envelopes) and a published and a
  staged selection of `min(each, REC_SEL_MAX)` entries. The staged rows are what lets a partial
  transaction leave the last whole selection in force, so the cache holds two selections per
  actor where the plan counted one.
- **What the codec checks**, the same in `tagpu_recmp_pov_put` and `recmp_file.py`, with a
  refusal leaving the cache unchanged:
  - the envelope's own fields, and its origin's source roster announced on the same stream;
  - provenance per kind: a later sequence, and neither the source tick nor the application tick
    back;
  - a sample of exactly 32 bytes passing `rec_pov_ok`, a click of exactly 20 passing
    `rec_click_ok`;
  - a selection chunk of exactly `8 + 6*count` bytes with flags within BEGIN|END, a total of at
    most `min(each, REC_SEL_MAX)`, `first + count <= total`, slots strictly ascending across the
    transaction and inside the actor's roster block;
  - an empty chunk only as the empty selection (total 0, BEGIN|END); END exactly at the total,
    any other chunk short of it;
  - BEGIN at entry 0 with nothing staged; each further chunk on the next sequence, at the same
    source tick, with the same total, starting where the last ended. Only END publishes.
- **The seal** is refused while an origin's selection is partial, beside the order and
  exploration transactions, in the history, the recorder's receive loop and Python.
- **The sampler's rule is the codec's** (`tagpu_recmp_pov_due`), so the room skip is tested
  without a game: the last sample is the cache's latest for the origin, and a skipped sample leaves
  it unchanged. `tagpu_recmp_room` is `256 - (published - read)` on the transmit queue.
- **A selection is one encoder call** (`tagpu_recmp_select_encode`): chunks of 670, 670 and 160
  for a 1500-unit block, each published and put before the next, the entries checked as put checks
  them before anything is emitted.
- **The capture** runs after the explored map in `mp_capture`, for each seat whose controller is 1
  and whose origin is a roster actor of that seat (any other refuses the contribution). It samples
  at `RECMP_POV_EVERY` (6); the selection walk is `sel_walk` under the roster's units per player,
  and a refused walk or a walked slot outside the actor's roster block refuses the contribution,
  with a log line. Unlike a skirmish's, whose walk refusal only stops recording the selection,
  a contribution's refusal ends all of them: a refused walk means the block or the births column
  has gone, which the roots refuse as well.
- **A network click** is published from the redirect at the channel's capture tick `s_mp_tick`
  once `s_mp_advanced` is set by the first advance, on the contributing game thread only. A refused
  publication sets `s_mp_pov_failed`, which the next B2 and the level's end turn into the
  contribution's refusal. An Interface Type above 1 is skipped with one log line.
- **Log lines**: the first envelope of each kind per origin, `recmp: <local|remote> <camera
  sample|click|selection> ID <origin> source tick <t> seq <s> applied tick <a>, <n> bytes`.
- **The playback**:
  - `mp_authority` copies each actor's samples (their application ticks as the samples' ticks)
    and selection from the reader at its tick, after the explored map, and `mp_rebuild` clears
    them before it, so a seek rebuilds them as it rebuilds the map.
  - `persp_world` gives Follow, the overlays and the HUD's rows a copy of the reader's world with the
    composed units, lists, samples and selections. Its `tick` is the reader's, the application
    tick the world holds, which is the displayed tick n.
  - A click goes into the ring from `mp_apply_held`, at its envelope's application tick, unless
    the step composes for a seek or the envelope is a proof record. The step's root-batch split
    (`mp_post_capture`) tests for kind 10 only, so it already passes kinds 13-15.
  - A selection entry is drawn while the composed unit is live with bit 28 and the composed
    lifecycle's birth for the slot is the entry's (`ov_born`).
  - A multiplayer file's takeover timeline is a skirmish segment, and there `persp_world` is the
    reader's own world, as for any skirmish.
- **Tests** (2026-10-06, no game run):
  - `tools/replay/recmp_pov_test.py` checks samples, clicks and selection chunks: every malformed
    length and field, provenance, the 670-entry transactions, partial and interrupted ones, the
    encoder's refusals, a seal while partial through the history, and a room-skipped sample.
  - `recmp_file_test.py` sends a sample, a click and two selections through the real channel,
    writer and reader. They are absent at 100 and present at 101, the same after both seeks, and
    the C and Python histories agree on all 34 envelopes. Seventeen mutations of kinds 13-15 are
    refused by both, the seal over a partial selection among them.

Gaps 4c-4 leaves:

- **V4** ran 2026-10-06 on a two-peer recording whose joiner clicked, dragged, scrolled and
  zoomed: the file validates in C and Python, the playings in full view and from the joiner's
  seat are EQUAL at 300 and 1198..1203, and Follow of the joiner at GameTime 1272 put the eye at
  (10295, 4738) and the level at 76205/65536, exactly the file's sample (camera minus half the
  view; zoom 76205). The viewer's screen was the joiner's size, 896x704, so the fit of another
  size is unmeasured on a network recording.
- **Every peer needs this build**: a peer without kinds 13-15 refuses them as invalid data.
- **The physical budget** (owner Q3) is still unmeasured.

## Landings

| Landing | Delivers | Review |
|---|---|---|
| **4c-1 Skirmish capture** | `tagpu_recpov.h`; `RK_POV/CLICK/SELECT/POV_AT` and `REC_FORMAT` 7 in `tagpu_recfmt.h`; the B2 capture and the two click redirects in `tagpu_recorder.c`; the world copy and snapshot in `tagpu_recwriter.c`; `RecWorld` state, `recr_tick` and `recr_seek` in `tagpu_recreader.c`; the `tarec.py` mirror and its `pov` command. `tarec`'s `RK_EV_NET_CREATE` test becomes `format >= 6` | high: two new byte patches |
| **4c-2 Follow, Jump, Free** | `tagpu_persp.c` (clock and follow arithmetic, with a native test like the cards'), the follow lever and apply in `tagpu_zoom.c/.h`, the packet and command fields, the cards' Jump, Follow and Free cam, and the tacli triggers | high: game/render handshake |
| **4c-3 Overlays** | the overlay builder, `PK_OVERLAY`, `rsel` and `ov_col`, `tagpu_native.c`'s selection rectangle, `tagpu_mark.c`'s cursor and ring, the switches and the dock's Overlays menu | high: packet layout |
| **4c-4 Multiplayer** | kinds 13-15 end to end, as in the section above | high: wire kinds |

Each landing runs `tareplay.py landing`. A new fixture, `perspective`, reaches the new functions.
Its scenario, `scenarios/replay-perspective.json`, centres the unpinned camera on the human's
commander and puts two Peewees inside the drag box. Its `keys` steps are `pmove`; a left
`pclick` on the commander at the window's centre; a drag selection (`pmove` and `down:lbutton`,
`pmove`, `up:lbutton`, each a step of its own so the engine sees the move between the press and the
release); a Shift `pclick` on the commander (`down:shift` … `up:shift`, which the shield turns into
`MK_SHIFT`); a `prclick` on the ground, which orders the three to move; a left `pclick` on empty
ground, which deselects under Interface Type 1; an arrow key held for 60 ticks; and `wheel:1`. The
injected `pclick`, `prclick` and `pmove` tokens are used rather than `click`, `rclick` and
`mouse:`, which fall back to `SendInput` and the desktop's pointer when the shield is down.

**The steps are 90 ticks apart, and the gate's points start at 721** (the fixture's `first`).
MEASURED 2026-10-06:
- **Input timing.** At the test speed a step costs about 60 ticks. Two `tacli keys` calls that land
  before the game reads its keys file are injected as one burst; the runner saw `input: keys
  prclick:700,500 pclick:850,150`. The live loop's first step fires near GameTime 160.
- **Why the drag needs separate steps.** A mouse move is not queued. `WM_MOUSEMOVE` replaces the
  input object's current record, and only button messages enter the ring (`0x4C2360` at `0x4B5EA4`
  against `0x4C2E30` at `0x4B5F51`). So a press, a move and a release sent in one burst reach the
  engine as a click at the release point.
- **The drag as built.** The drag is three steps, each landing in a read of its own.
- **Why the gate starts at 721.** A selection held by the recording machine's human is that
  machine's interface state: the unit panel, the desktop GUI `main+0x561`, the game-screen state
  `main+0x37E9E`/`+0x37EC1` and the cursor `0x51F4AE`/`0x51F4B2`. These 8 bytes failed the gate at
  391 with a selection held from 224. The null twin classifies what a viewer does without changing
  the game, and selecting is not among it.
- **What `first` does and does not change.** The selections are made and cleared before
  GameTime 721, and `first` keeps every gate and round-trip point after them. It moves where the
  checks run, not what they check: no byte leaves the gate.

The drag is no click record: it selects through the drag box,
not through either click action (MEASURED: two clicks for a click and a right click with a drag
between). It
sets `"watch": true` and ends at tick 1500.

**Verification runs** (each 10 minutes or less, private display, retail):

- **V1 (4c-1).** `tareplay.py run perspective`, then `tarec.py pov FILE --check`:
  - **Structure:** lengths, enums, spacing of at least `every`, a sample only on change, and
    selections ascending inside the block.
  - **Seek equivalence:** forward state equals seek state at every snapshot and at 8 random
    targets.
  - **Exact expectations:**
    - the selection after the click names the commander's slot and birth (read with `tacli peek`
      while paused);
    - four clicks (left, Shift left, right, left) with the injected buttons and keys at
      `main+0x2CAA`'s integer parts, and selections of 1, 2, 3 and 0 units;
    - `cam_x` grows across the scroll;
    - `zoom` equals the wheel's logged level × 65536, rounded.
  - **Gate:** the fixtures gate `tareplay.py landing` passes, with the changed recorder, writer
    and reader functions reached.
- **V2 (4c-2).** The W leg of `perspective`:
  - **Follow while paused:** seek to `T1` paused and follow seat 0. The peeked eye and target
    equal `tarec pov --eye T1 --viewer W,H` exactly, and the `replay pov` level equals the
    expected Q16.
  - **Follow while playing:** play 3 s, pause, and compare again.
  - **Scroll:** the arrow key logs "follow ended: camera moved", the cards show Free, and the eye
    has moved.
  - **Wheel:** follow, then `wheel:1`, logs "follow ended: zoomed", and the level starts from the
    follow level.
  - **Seek:** follow, then seek the dock to `T2`; the eye equals the expected value at `T2`.
  - **Jump:** jump 0 puts the eye and level at their expected values and leaves Follow off.
  - **Device clicks** on Follow, Jump and Free cam at HUD 1x.
- **V3 (4c-3).** Paused 2 ticks after a recorded click, Vulkan at 1280×720, zoom 1.0 and 0.5
  (`tagpu_zoom.txt`), then `tacli` capture and pixel probes:
  - **Colour:** the cursor tip, a ring pixel and a selection-rectangle pixel are the seat's
    palette colour at the projected positions.
  - **Switches:** each switch off restores those pixels, with the change confined to that
    overlay's bounding box.
  - **GDI** (`tagpu_gdi.on`): the playback runs, draws no overlay and shows no error box.
- **V4 (4c-4).**
  - **Codec tests:** C `recmp_pov_test.c` (malformed lengths and enums, chunked transactions,
    seal while partial, a room-skipped sample) and the `recmp_file_test.py` additions, about
    2 min.
  - **Two-peer run:** owned Wine peers with native DirectPlay, the host recording. The joiner
    holds the eye, scrolls, `pmove`s, `rclick`s and selects. `recmp_file.py` validates kinds
    13-15 with source and application provenance, and the C history agrees.
  - **Host playback:** following the joiner's seat gives the eye computed from the file's
    application ticks, and the joiner's selection rectangle appears.

## The owner's answers (2026-10-06)

1. **Follow on another resolution:** the recorder's visible area, the zoom fitted to the viewer's
   screen.
2. **GDI:** overlays on Vulkan only; GDI shows Follow's camera and no overlays (a named gap).
3. **Multiplayer rate:** camera and cursor at 5 Hz in multiplayer (`RECMP_POV_EVERY` 6), 10 Hz in
   a skirmish. The physical-budget measurement stays open (stage 6).
4. **The switches:** beside the hide-chat switch (the owner's choice for hide-chat, 2026-10-06):
   then in the watching dock's Overlays menu, and since the replay HUD (owner, 2026-10-08) the
   four buttons of its foot plate, each with its own icon.
5. **Whose overlays:** every recorded player's at once, independent of Sight.
6. **Issued orders:** the engine's order markers from the recorded queues, already done; nothing
   further.

No loosening (CLAUDE.md's list) and no escalation reason arises in this design.

## Gaps this design leaves

- **Delay:** camera and cursor play `every` ticks (0.1 s in a skirmish) behind the world.
  Clicks do not, so a ring can appear just before the cursor reaches it.
- **Zoom below 1:** a pointer in the display-only ring records the nearest point the engine can
  name [INFERRED from `tagpu_zoom_to_engine`'s ring].
- **Input modes 2-5** (E11) may leave the world point stale; this is not established.
- **Pauses:** changes made while the game is paused collapse onto the paused GameTime.
- **Multiplayer pacing:** motion follows when the recorder received each sample.
- **The shake** is not reproduced.
- **Viewed blocks:** selections made in a viewed (`+view`) block that is not the controlled one
  are not recorded.
- **GDI:** no overlays (Q2).
