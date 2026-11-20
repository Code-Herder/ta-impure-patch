# Build scatter: building where your own units stand (plan)

A building can be placed on ground the player's own units stand on, and when the builder gets there
they move out of its way. Inspired by TADR's *construction kickout*, and built on a different
footing: a manager that owns the moves it gives, in the manner of [the factory guard's
coordinator](factory-guard-plan.html). The module as built is [build scatter](modules/build-scatter.html).

## What TADR does, and what this does differently

TADR's `ConstructionKickout`
([source](https://github.com/tanvanman/TADR/blob/dcff5ddeb6bd1030e3f452c0f16e5f005850f62f/src/DDraw/ConstructionKickout.cpp))
hooks the same refused branch of MobileBuild, `0x403CD0`, and its VTOL twin `0x41400D`. Inside the
builder's order handler it finds the local player's units on the footprint and rewrites the order
list of each -- all but a unit moving to a point off the footprint, and a builder working alone on
a young frame -- to a move in a direction drawn from `rand()` (or to the side of its attack target),
out to a distance scaled by the footprint. It doubles the abandon's count (`0x403D05`, `0x414046`, 10 → 20)
for every builder, lets the cursor through with a sentinel written into the cursor's `push 0`
(`0x4198C3`) and a hook on the yard test's occupant compare `0x47D554`, and colours the square by
rewriting a code byte of the draw (`0x469E7F`) from inside the test.

| TADR | here |
|---|---|
| order lists rewritten from inside the builder's running order handler, while the controller walks a list ([the port's evidence](tadr-port/sim-fixes-evidence.html) records a crash of TADR's own that this re-entry caused: a cancelled build order's destructor ran its handler inside the outer one) | the handler's stub writes the module's own table; the moves are given after the unit tick, through the root a click uses |
| a unit attacking, guarding or building on the footprint is moved, its order replaced | idle units only (decision 1) |
| a random direction: not repeatable, and the place may be taken, unreachable or another site | the nearest place the unit's class can reach and stand on, clear of structures, sites and the places given to the others; no random draw, so a recording replays it |
| a move the player gives onto the footprint is replaced at the builder's next test | a row per move, by value; a player's order frees the row and runs to its end, and the unit is moved again only once it stands idle on a refused site |
| the abandon delayed for every builder, blocked by anything | a refusal does not count only while units are in fact getting off the site |
| a code byte rewritten per test for the colour | a byte in the frame packet |

## The owner's decisions (2026-11-19)

1. **The first landing moves idle units only**: a unit whose order list is empty or holds the
   engine's own stand-by. There the move is exactly a player's click on an idle unit. A unit passing
   over the site leaves by itself; a unit standing on it under an order is a later landing.
   *Widened 2026-11-20 after the owner's first game* (a bunch of Jeffys read red): a unit standing
   still under a lone move the engine has failed counts as idle too. Stated to the owner as a
   widening of this decision when it was made.
2. **Placing under your own units is in**, with the **yellow square** for a site that will be cleared.
3. **Builders seated by the factory guard on a queued site are re-seated by the guard**: the scatter
   manager marks its site as taken ground for the guard's planner, so one manager orders a unit,
   never two.
4. **A player's order always wins.** A row owns the one move it gave, never the unit: an order
   given by hand to a unit that is moving off -- further out, or back onto the site -- replaces that
   move and ends the row. A unit that then stands idle on a site its builder is still refused on is
   moved again. *(The second sentence is the implementer's reading of "handled gracefully"; the
   alternative, leaving a hand-placed unit alone until the builder gives up, was stated and not
   chosen.)*
5. **A unit that dies while moving off, a builder that dies while waiting and a wreck left on the
   site are test cases, not hopes.** Rows name units by slot and birth and keep no address; every
   use is re-validated; the test group plays all three.
6. **Safe by construction**: no order list is touched from inside an order handler.

## Landings

| | what | state |
|---|---|---|
| 1 | Idle units are moved off a refused site; the cursor accepts a site under them and draws it yellow; the test groups | built 2026-11-20 |
| 2 | The factory guard's seated builders on a site (decision 3), and scatter destinations kept off the guard's lanes and seats | to do |
| 3 | Units standing on the site under an order (decision 1's later landing): which orders may be interrupted, and how the unit takes its order up again | open: needs the owner's rule |

## Landing 1 as built

The mechanism is the module note's. What was decided while building it:

- **The tick site is the players phase**, the call at `0x49555F` that allied sight already owns
  (`players_phase`): after the step's unit tick, projectiles and explosions, on the game thread,
  once a step. A click's orders are made between two steps on the same thread
  ([orders](engine/orders.html), *Calling the order roots from the AI's tick*, states the same
  ordering for the AI's planners).
- **Idle is the head's bit and its action together.** `0x4000` alone also marks the attack an idle
  unit's reaction put in front of its stand-by, and a Paralyze; a plain move would cancel the fight.
  The action must be the def's default order `+0x230`, which the controller makes the node from.
- **The cursor's flag rides in the yard test's own argument.** The test reads its *ignore* argument
  as a word at one compare; bit 16 marks the cursor's second question and bits 17 to 20 carry the
  placing seat. Nothing is stored between calls, so nothing can be left set. The seat cannot be read
  from the test's player argument at the compare: MEASURED, the first build read it there and the
  cursor stayed red, because the test has reused that stack slot by then (`0x47D392`, `0x47D3DF`,
  `0x47D49D`).
- **The abandon is held by taking one off the retry counter**, not by a longer limit: a builder
  whose site is blocked by something that will not move gives up exactly as stock.
- **Human seats only.** The stubs would serve any local seat; an AI's are left out so that the stock
  AI's base-building, and every game ImpAI is tuned and tested on, stay as they are. Serving ImpAI's
  seats is a decision for its own plan.
- **The builder walks off its own site.** The cursor accepts a site under the selected idle
  builder; MobileBuild's approach moves it off before the test and no refusal is seen (MEASURED,
  below).
- **The hold has a bound**, 20 held refusals a site, so that no traffic across a site can keep a
  builder waiting for ever (the landing review's finding: the first cut rested on units stopping).
- **Nothing installs without its tick.** The tick's one caller is allied sight's players phase; the
  module asks for it first, so the cursor never accepts a site nothing would clear (the review's).
- **The stock-limits build runs the feature too** (unit births are known there), so the twin test
  group plays the same steps and the same assertions.

### MEASURED 2026-11-19/20

`scenarios/build-scatter.json`, Two Continents, 1024x768, the reference setup. By hand at game speed
10 and through `tools/replay/tareplay.py run build-scatter stock-build-scatter` at game speed 20;
GameTimes are the harness's, identical on the raised and the stock-limits build.

- **The cursor.** A solar collector over four idle ARMPW: stock's answer 0, `main+0x2CC6` reads
  `0x46` (bit 6 set) with the module, and the Vulkan square is yellow there, green on free ground
  and red over a tree (a window capture of each).
- **Site A.** Ordered at GameTime 140, the ARMCK is refused first at 332: `site builder=2 type=112
  cell=57,470 5x5 on=4 moved=4 noway=0 busy=0 other=0 full=0`. The four end on four different places
  round the site and the solar is built.
- **A player's orders** (steps at 333 and 334). The ARMPW sent to (1000, 7300) stands there at 804.
  The ARMPW sent back to the site's centre is left alone at the refusal of 362 and moved again at
  392: `on=1 moved=1`.
- **Site B.** Its builder is refused at 796 (`moved=4`). At 806 every unit of the game is removed
  while the four walk and the builder waits: no fault, and the next site's plan is unaffected.
- **The wreck.** A third site is accepted by the cursor at 856 and a wreck dropped on its middle at
  866. The builder is refused at 1048, its four units are moved (`moved=4`) and stand off the site,
  no solar exists at 1600 and the builder's order list is headed by its default order: abandoned as
  stock.
- **The builder's own site.** A solar placed on the selected ARMCK (`main+0x2CC6` `0x46`): the
  builder walks 50 px off, the solar is built, no `scatter:` line.
- **The bunch** (the owner's game, 2026-11-20, GameTime 21 093 to 23 983). Ten ARMFAV sent together
  to one point: eight on Standby, two still holding the lone move with `0x800000`, unmoved for
  12 000 ticks; the square over them was red with the first build, since those two were busy by the
  first rule. In the test group site B's four ARMPW are sent to its centre at GameTime 121 to 124
  and the cursor is asked at 420.
- **A unit that cannot leave.** In a hand run the wreck was dropped over one of the units: that unit
  stayed, its row stopped holding the builder after 180 ticks, and the builder gave up. No fault.
- **Replay.** Both test groups pass: the record, the puppets, three round trips and the seek each,
  with no COB or native write on a puppet.
- `python3 tagpu/ddraw/tests/scattertest.py`: `ok (0 failed)`.

### Not closed by landing 1

The module note's *Gaps*, and: no run in a network game (the orders are local and no order crosses
the wire, as for a click, but no two-player game was played); an air builder's twin branch
`0x41400D` is installed and byte-matched but no game exercised it.
