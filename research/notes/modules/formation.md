# Formation move

A drag with the button that gives orders spreads the selected units evenly along the dragged path
instead of sending them all to one point. A port of BAR's *Custom Formations 2* widget, Move only.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`. The engine side is in the engine map: the click routing and the clicks in [input](../engine/input.html) (*The in-game mouse buttons*, *The placement's mouse path*), the order roots and the resolver in [orders](../engine/orders.html), the move sprite in [render](../engine/render.html) (`0x439740`).

## What it does (`tagpu_formation.c`, `tagpu_formgeo.c`, `tagpu_route.c`)

With mobile units selected, the player holds the order button and drags. Each mouse move over the
world adds a point to a freehand path; at the release the path gets one slot per unit, spaced
evenly by arc length with both ends included, and each unit is sent to one slot. With **Shift
held at the release** each unit's slot is appended to its own queue, and the assignment measures
from where that unit's queue ends; without Shift each unit's orders are replaced, as a stock click
replaces them. A drag shorter than the threshold is stock's click at the **press** point. While the
button is held, a dot — the engine's move waypoint sprite — marks each slot the release would
order.

**The gesture, by Interface Type** (`main+0x37EFA`; 0 is left-click, 1 right-click):

| | Interface Type 1 | Interface Type 0 |
|---|---|---|
| nothing armed (mode 1), right drag | **formation** | stock: the right press deselects at the press |
| Move armed (mode 2), left drag | **formation** | **formation** |
| nothing armed, left drag | stock: the selection box | stock: the selection box |

A press starts a gesture only when every unit it would order would move (`admit`): the pointer on
the world view (`main+0x2CC6` bit 1) with no box open (bit 3); the other button not held; **no
unit under the pointer** (`main+0x2CBA` zero, so attack, guard, load and repair over a unit stay
stock's); in mode 2 a cursor stock's click issues an order with (`main+0x2CBE` below `0x11` and
not `0x0F`, the test at `0x499027..0x499044`); the point inside the map; and the resolver
`0x43F0E0`, asked for every selected unit of the controlled player with the press's point,
answering `MOVE_GROUND` or `VTOL_MOVE`, or in mode 2 `QMOVE` (a factory's rally), or nothing (in
mode 1 a unit with no mover is given nothing by stock either). Any other answer — a reclaim of the
feature under the pointer, say — leaves the press to stock, unchanged. So does a right press
that Quick Build's gate on `0x499100` would turn into a build (`tagpu_resource_quick_right`, the
same `quick_choice` the gate and its build ghost use): a right drag that starts on a metal or geo
spot with the ghost showing builds there, as a click does. A refused press writes nothing.

An admitted right press is **deferred**: stock gives the order on the press (`0x499100` →
`0x48CF30` at `0x4991F5`), and with formation on that order is given at the release instead.
That holds for every right-click order a Type-1 player gives on the ground with movers selected,
including a plain click, whose stock order is then made at the release by stock's own function
(below).

**At the release** (`commit`): the path takes the release's point; a path shorter than the
threshold replays stock's click with the press's record and fields. Otherwise the participants
are re-derived from their slots (still the player's, alive, selected), each one's start taken —
its position, or under Shift where its queue ends — and one left with a start outside the map
dropped. One participant or none left: stock's click at the release point (BAR's single-unit
rule). Two or more: the slots, the assignment, and one order per unit through `0x43ADC0`. In mode
2 each QMOVE unit gets `0x43AFC0`, the root stock's click calls (`0x48D1F4`), with the path's end
as its rally, and then stock's click tail `0x4958C0(shift)`: without Shift Move disarms (mode 1, bit 5 cleared, STOP's radio
reset); with Shift `main+0x2CC6` bit 5 keeps it armed until Shift is released (`0x4992D8`).

**A Shift-kept Move outlives its own disarm.** With bit 5 set at the press (`keep`), the handler
disarms Move at the first record that finds Shift up (`0x4992D8..0x49930F`: mode 1, bit 5
cleared), before the routing sees that record. Stock had already ordered at the press, so the
gesture carries on (`shed`, `still_armed`): its moves are tracked, and its left release —
arriving in mode 1, past the click route, so taken at the right-press stub and then handed on to
stock's mode-1 path as stock's own release was — commits with Move's orders and without the tail
the engine has already run. A short or single release then calls what `0x498F70` calls in mode 2,
`0x48CF30(rec, 2, 0, main+0x2CAA, 0, 0)` (`0x49906D..0x499087`), under the same field swap; it
records no click. The commit's log line says `shed=1`.

**A gesture ends without an order** (`drop`, logged with its reason) on: a press of the other
button (BAR's cancel; in mode 2 the right press then cancels Move, as stock), a second press of
the gesture's own button (a release the handler never saw), a move without the gesture's button
that is not the stale pre-press move IdleTick re-reads (`tagpu_route_same_move`; *After a press,
IdleTick re-reads the move made before it* in the engine map), a record handled in another cursor
mode or Interface Type (the handler's entry observer; the shed Move above aside), and a release whose world view the router
refuses (`drop reason=world`). The level's end clears a gesture too. Stock would have ordered at
the press; a deferred press whose release never arrives orders nothing (owner's decision).

**The click threshold** is BAR's (`MouseDragFrontCommandThreshold` and the widget's
`minFormationLength`), fixed, with no setting: `base = (16·H + 540) / 1080` presented pixels,
`H` the presented height (`g_ddraw.render.viewport.height`), clamped to 8..40, plus 16; converted
to game pixels through the mouse's unscale (`g_ddraw.mouse.unscale_y`) and to world units through
the zoom (one game pixel is `1/zoom` wu, `tagpu_zoom_to_engine`); never below 20 wu. So 32
presented pixels at 1080 lines, 48 at 2160. TA's projection is orthographic (`0x469C63..0x469C94`),
so on level ground one game pixel is one world unit on both axes. It is asked at the release
only, against the path's length.

## The seams

| site | what | thread |
|---|---|---|
| `0x49939B` (`tagpu_route.c`, `install_right`) | **The right-press branch's stub**: 12 bytes, `E9 rel32` and seven `90`, written at attach after the 18 bytes `0x49939B..0x4993AC` up to the call's `E8` and the `jmp` at `0x4993B1` match (the call's rel32 is not matched: the demo recorder rewrites it). `pushad`, `right_button(record copy, message)`, `popad`; 1 → the frame `0x4995B8`, 0 → the three original instructions and on to `0x4993B6` or `0x4993A7`. Every record the handler does not divert before it passes here, in every mode: the press is deferred, the moves are followed and the right release commits (`tagpu_formation_right`) | game |
| `0x4993BF` (`tagpu_route.c`) | The router's click-route stub, shared with the placement and the reclaim sweep: in mode 2 it asks `tagpu_formation_button`, which admits a left press (1, consumed) and commits a left gesture's release (1); everything else 0, the original instructions | game |
| `0x499200` (`tagpu_route.c`) | The router's before-observer on the handler's entry: `tagpu_formation_entry` drops a gesture when the record is handled in another mode or Interface Type | game |
| `0x4993AC`, `0x4995B3` | Read at attach, never written here: each `E8 rel32` is decoded to the function stock's call reaches — the recorder's click stub when it installed, else `0x499100` / `0x498F70` — and a replayed click calls that target | game (attach) |
| `0x43F0E0`, `0x438760` | The resolver, at the press, for every selected unit; the name search for the three move actions by the engine's own strings (`0x5052E4` `MOVE_GROUND`, `0x5016CC` `VTOL_MOVE`, `0x50531C` `QMOVE`), once | game |
| `0x43ADC0`, `0x43AFC0`, `0x4958C0` | The commit's roots and stock's click tail (above) | game |
| `0x469D2C`, hook 9 (`tagpu_markown.c`) | The marker block's end: on GDI the dots are blitted into the engine's frame here, `0x4B8500` per dot | game |
| `0x4B8500` | Called by `tagpu_formation_gdi` with the move cursor's frame, as `0x439740` calls it at `0x4399BB` | game |

`tagpu_formation_init` runs after `tagpu_route_init` and the recorder's attach (`dllmain.c`) and
arms only when the three router sites it needs are installed and both click call sites decode;
`formation: armed (right 0x49939B, route 0x4993BF, entry 0x499200; clicks …)` or `formation: not
armed (…)`. Unarmed, every entry point answers stock.

## The invariants

- **Off is stock exactly.** With `tagpu_formation.on` false no gesture starts, every record is
  answered 0, and both stubs run the engine's own instructions.
- **A replayed click is stock's click** (`replay`), an ordering: it runs on the game thread
  inside the handler for the release record, after `0x498DA0`, the hover query and the cursor
  chooser have written that record's fields and before anything else of the handler reads them.
  It writes over them the fields of the record it stands for (the press's for a short drag, the
  last record in the world view for a single unit) — `main+0x2C8E`, `+0x2CAA`, `+0x2CBA`,
  `+0x2CBC`, `+0x2CBE` and bits 0..2 of `+0x2CC6`, exactly what those three producers write for a
  record ([input](../engine/input.html), *The record and the world point*) — calls the decoded
  target with a copy of the record, and puts the release's fields back. Both clicks read the
  record only at `+8` (`0x49917A`; `0x498F70` passes it on); the one arm of either click that
  writes one of those fields, `0x499100`'s Interface Type 0 minimap arm (`+0x2CBE`, `+0x2CC6` bit 4,
  `0x49919D..0x4991B4`), is never reached by a replay, since a right gesture exists only at Type 1;
  the render thread reads none of them (it reads the frame packet), and the recorder leaves
  `main+0x2AA7..+0x2CC6` out of a recording (`replay-globals.tsv`). Quick Build's raw point is
  re-sampled around the call (`tagpu_resource_mouse`) only where vpwide's redirect sampled it for
  the record in the first place.
- **The call-site targets are fixed for the process.** The recorder is the only writer of the two
  rel32s and writes them in `tagpu_rec_init`, which `DllMain` runs before this module decodes
  them; nothing writes them after attach.
- **Every unit is bounded before each root call.** Re-derived from its slot (`tagpu_route_unit`
  bounds the slot by the array's own count; the owner byte, the selection bit, the def index
  below `UNITINFOCount`); no node pointer is held across a call; a Shift append is refused when
  the unit's list holds 1024 nodes (`FORM_LIST_CAP`, below the recorder's 4096,
  `REC_ORDERS_MAX`), counted `listcap=`.
- **Where a queue ends** (`queue_end`, Shift's start) mirrors the order-marker walker `0x439B30`:
  at most 16 384 nodes, stopping at a node that is not the unit's (`+0x0E`); a node counts when
  its action is below the descriptor vector's own length (`[0x512344]..[0x512348]`, stride
  `0x19`) and its descriptor's marker mask `+0xC & 0x0F` is non-zero; its position is its target
  unit's when `+0x16` names a live slot on the array's stride, else its own `+0x22`. The last one
  wins; none, the unit's own position.
- **No sound call.** `0x43ADC0` without Shift sets the node's `+0x42` bit `0x2000` (`0x43AEE7`),
  and the move handlers answer it with the unit's "ok" through `0x438880` (Move_Ground at
  `0x40322D`, VTOL_Move at `0x40FA6B`), whose reply queue refuses a kind already queued
  (`0x47FB0A`): one "ok" for the group, none under Shift, as a stock click.
- **No order goes to a peer**, from either path: neither `0x48CF30`'s call list nor `0x43ADC0`'s
  holds a send. A peer sees the units move as it sees a stock click's.
- **The commit allocates nothing**: its working set and the geometry's scratch are static, used on
  the game thread only.

## The geometry (`tagpu_formgeo.c`)

Engine-free, Win32-free and **integer only**, so `tools/formation_regression.py` reproduces every
result bit for bit to predict a drag's orders. Points are 16.16 world coordinates, x and z in
`[0, 32767]` wu, so every delta is below 2³¹, every sum of two squares below 2⁶³, and int64 holds
every product.

- **The path**: a point at the last node's x and z is skipped (BAR's duplicate); a full path
  (4096 nodes) keeps its even nodes and its last before the next point, so the first and last
  nodes always survive. Its length is the sum of each segment's `floor(sqrt(dx² + dz²))` in 16.16,
  the square root digit by digit.
- **The slots**: `n` of them at `t_k = L·k/(n−1)`, linear between the two nodes `t_k` falls
  between (x, altitude and z alike), both ends exact.
- **The assignment**, the cost a unit-to-slot distance in whole world units rounded to nearest
  (BAR's `floor(sqrt + 0.5)`), capped at `0x7FFF`:
  - **up to 256 units, optimal** (`form_hungarian`): the O(n³) shortest-augmenting-path
    algorithm with potentials over a static 257×257 int32 matrix (264 KB); every potential stays
    within `n·0x7FFF` < 2²³; a tie goes to the lowest slot.
  - **above 256, bounded** (`form_nox`): units and slots sorted along the direction of the
    furthest pair (the starts first, then the slots; `(1, 0)` when every point coincides), with
    the index as the tie-break so the order is total, and paired in order; then at most 4 passes,
    and 2 000 000 segment tests in all, swap the slots of two units whose paths cross properly.
    Only a proper crossing swaps, so each swap shortens the pair's total; the work cap bounds the
    passes whatever happens. At 1500 units one pass is 1 124 250 tests, so the cap ends the work
    inside the second pass: above 256 units the answer is deterministic and bounded, and **not
    always free of crossings**.
- **Cost, MEASURED 2026-11-09** with `tagpu/ddraw/tests/formtest.py --wine bench` (the DLL's
  compiler and flags, under Wine; random starts, a line and a ring of slots): the Hungarian at
  n = 256, 7–11 ms; the bounded assignment at n = 1500, about 20 ms. Both run once, on the game
  thread, at the release.

`tests/formtest.py` builds `tests/formtest.c` natively with the sanitizers and runs hand-worked
checks of each function; `formtest.py dump` answers the cases `formation_regression.py
--selftest` generates (3000 random cases, plus 255, 256, 257, 300 and 1500 units and paths past
the decimation), compared line for line.

## The preview

The dots show from the first drag move that adds a node — a path of two nodes — while at least
two of the press's participants are still alive and selected; one dot per live participant, at the
slots the release would use (the assignment is not run until the release). A release under the
threshold is still a click, and the dots go with the gesture.

- **Vulkan.** `tagpu_formation_dots` copies the dots by value into the frame packet's
  `PK_FORMDOT` table (`TAGPU_PK_MAX_FORM` 1500; `TAGPU_PK_TRUNC_FORM` when cut), whose acquire
  bounds the count and every coordinate to `±TAGPU_PK_PLACE_COORD`. The marker pass draws the move
  waypoint sprite (`cursor_ary[14]`, the Move descriptors' cursor index) at each, with the
  engine's frame rule and the order sprites' opacity (`ART_SPRITE_ALPHA` 0.70), one texel a screen
  pixel at every zoom (`tagpu_order_formation_gather`), into the order art's bucket — 6000 quads
  shared with the order markers, drawn fogged as they are. It runs under the cursor layer's gate,
  placement's (`tagpu_mark_cursor_ours`: `mark.on`, not `nocursor` or `passive`), and needs
  `order.on`: the art atlas is the order module's, and the packet's fill builds it when a dot rides
  (`tagpu_order_art_ensure`), because the order snapshot builds it only behind the engine's Shift
  gate.
- **GDI.** At hook 9, while the marker pass does not draw the cursor layer (`g_cursor` clear),
  `tagpu_formation_gdi` blits the same frame into the engine's own frame with `0x4B8500`, as
  `0x439740` does: `cursor_ary[14]`'s frame `(GameTime / (2·duration)) % count` — the engine
  divides by both unguarded; here a zero count draws nothing and a zero duration counts as 1 — at
  `x − eyeX + 0x80`, `z − (altitude >> 1) − eyeY + 0x20`; `0x4B8500` clips. It needs `markown.on`
  (hook 9 is markown's redirect), which the play defaults arm.

The counters: `mark: … form=` (dots the marker pass drew this frame), `order: … form= formover=`
(drawn and refused by a full bucket), and at the level's end `formation: level end commits=
short= single= drops= listcap= gdi=`. Each commit logs `formation: commit button= mode= itype=
units= qmove= nodes= len= thr= shift= algo= issued= listcap=` (`algo=h` the optimal assignment,
`x` the bounded one), a short drag `formation: short`, a single unit `formation: single`.

## The toggle

`formation=on|off` in `impure.cfg` (`TS_FORMATION`, default on), the in-game options panel's
*Formation move* row (Off / On; the panel's alone — the front-end Visuals screen has no slot for
it, [settings](settings.html) §2.12), and the levers `tagpu_formation.on` / `.off`. The play
defaults' precedence (`tagpu_opt.c`): an `.on` file, an `.off` file, the store, then
`tagpu_defaults.off`, then on. So players and `--defaults` launches have it on, and a bare tacli
launch, whose store is ignored, is the stock control. It is read at each press: a change applies
from the next press, with no restart.

## Replay, puppets and multiplayer

The orders a commit makes are engine state the demo recorder keeps, and are replayed as such; **a
commit records no click event**, so a replay's click overlay shows nothing for it (the owner's
decision, as for the placement and the reclaim sweep). A short drag and a single-unit drag go
through the click's call-site target, so the recorder records them as stock clicks, at the
release's tick. On a puppet seat the router's world view refuses the press
(`tagpu_route_world`), so a playing's puppet never starts a gesture. `formation-basic`
(`tools/replay/fixtures.json`, scenario `scenarios/formation-basic.json`: eight Arm mobiles and a
kbot lab on Comet Catcher) is the test group, bound to the module's sources, whose step runs
`tools/formation_regression.py <instance> replay`. Any test group that gives a Type-1 right-click
move under `--defaults` passes it through the deferral, since the module is on there.

## Gaps

- **Every Type-1 right-click order on the ground moves from the press to the release** while
  formation is on. The order is the same; its tick is the release's.
- **The threshold ignores slope**: off level ground one game pixel is not one world unit in z
  (BAR traces the terrain).
- **The bounded assignment above 256 units** is checked bit for bit offline only; the test
  group's scenario has eight mobile units.
- **Not ported**: patrol and attack lines, minimap drags, a waypoint path for one unit, BAR's
  attack/guard override over a hovered unit.
- **The dots outlive the gesture's input** while the handler receives no records (IdleTick skips
  it while `[[main+0xC]+0xF1] & 8`, `0x499A13..0x499A1A`), until the next record — as the
  placement's preview does.
- **Multiplayer** is parity by construction: every order is made by the root a stock click's
  reaches with the same arguments (`0x43ADC0`, as `0x43AFC0` forwards a no-Shift click,
  `0x43AFDA` → `0x43B08A..0x43B09D`). That no order root sends anything rests on a scan of their
  direct calls only (the destructor's table calls are not traced); no two-player game with
  the module on is part of its tests.
- **A side panel with no Move gadget gives no mode-2 gesture**; which supported mods' panels lack
  one is not established.
- On the Vulkan lane with `mark.on=nocursor` or `passive` the dots go to the engine's frame, which
  reaches the golden source and not the screen, as every engine-drawn marker does then.
