# GPU-only particles: the plan

On a Vulkan launch, the engine's particles stop being simulated, stored and copied on the CPU. Each of
the twelve live emitters records an **emitter event**, and the GPU draws every particle as a function of
its event and the game time. The engine's particle walker, its updates and its pool stop running, and
the pool's memory is given back. GDI, the oracle, keeps the engine's particle code. **Both renderers
draw particles from one random rule of their own**, so a GDI game and a Vulkan game of the same seed show
the same particles, and no simulation value depends on particles any more.

Owner's decisions, 2026-11-22: GPU-only particles from emitter events, all emitters; the vertex shader
over the pixel shader; option A for the random rule (below); recover the pool's memory.

## Why

In the 13 500-unit battle at 1080p ([zoom-out speed plan](zoom-out-speed-plan.md), item 5), the
particle tick costs 10.2 / 4.6 / 2.2 / 1.6 ms a frame (near busy, far busy, near thin, far thin) after
its prefetch. It is the walker `particle_walk` and the engine's updates `0x474580`, `0x4739B0`,
`0x475340` and the height lookup `0x485070`. On top of it, every frame the game thread copies the
particles into the frame packet (inside `publish`) and the render thread uploads them (inside `gather`).
Neither copy has been timed on its own. The pool is 204 800 objects of 76 bytes under the raised
limits: 15.6 MB, plus a 0.8 MB pointer stack, held for the whole process in the 32-bit address space.

## What the scout established

The facts behind this plan, every address cited, are in the scout's report
(`.data/gpu-particles/scout.md` in this worktree, gitignored; its corrections reach the engine map in
landing 0). The ones the design rests on:

- **The random stream is shared, and that is why the rule is option A.** The C runtime's `rand`
  `0x4E4870` keeps one seed per thread (`_tiddata+0x14`, TLS). The particle code draws from it at
  eighteen sites:
  - fire `0x472AB0`: `0x472ACF`, `0x472AF4`, `0x472B17`, `0x472B3A`;
  - flare spawn: `0x47395F`;
  - nano spawn: `0x473DAD`, `0x473DCA`, `0x473DE7`, `0x473E04`, `0x473E21`, `0x473E3E`;
  - wake spawn: `0x4749D9`, `0x474A00`, `0x474A27`;
  - the smoke spawns `0x474F1C` and `0x4752D4`;
  - the smoke updates `0x4753B9` and `0x475679`.

  The feature tick draws two more for a burning feature's smoke point (`0x424345`, `0x424379`). Two
  simulation values come later in the same streams:
  - the meteor strike points, on the game thread;
  - the random skirmish start positions, on the loader thread. A smoking map feature's endless smoke
    draws once at load (`0x472C50` → `0x4751C0` → `0x4752D4`) before the start shuffle
    (`0x4979E7`, `0x497A32`). That order is read from the static call sequence and is not yet measured.

  Stopping or changing the particle code therefore moves meteors and starts, unless particles stop
  drawing that stream.
- **Nothing else reads particle state.** No simulation, AI, network, save or sound code reads the pool,
  the layer table or the objects; scans of every reference show none. The recorder leaves the pool
  (`replay-globals.tsv`, `leave particlepool`) and has no row for the layer table. It records only the two
  nano emitters as `RK_EV_SPRAY` and re-makes the teleport flare from the recorded move. The replayed
  simulation fires the other ten emitters again. A seek empties the layers (`tagpu_effects_clear`).
- **The update is closed-form for four classes:**
  - fire, flare and nano particles move by a fixed step a tick and die at a fixed tick;
  - wake also dies where the ground under it reaches sea level, a terrain lookup along a straight path;
  - smoke drifts with the wind and draws one random number per frame advance, so it is closed-form
    given the wind's history and that random rule.
- **The pool can be emptied safely.** Our attach runs before the C runtime's initializer `0x471C80`
  builds the pool, and the renderer is final at attach. A capacity of 0 makes the constructor
  `0x470A90` skip the block allocation. The pool's alloc `0x470EB0` then answers 0, and every emitter
  skips its particle on that.

## The design

### The random rule (both renderers)

Built in landing 1 ([effects](effects.md) §7.7, `inc/tagpu_prand.h`). Every particle `rand` site above
answers `tagpu_prand(seed, particle, draw)` instead, in the CRT's range (0 to 0x7FFF): a 32-bit integer
hash of the emission's seed, the particle's ordinal within its object and the draw's number within that
particle. The engine's particle code, which still runs on GDI, and the GPU's closed form therefore
compute the same values by construction. Neither draws the CRT stream, so meteors and start positions
no longer depend on particles on either renderer, whatever was on screen.

- **The seed** of an emission is `seed(level, GameTime, n)`: the level's particle seed (made at the
  layer table's allocation, before the map's features spawn: the session and the map in a network
  game, the fixed seed and the map under `tagpu_seeds.on`, otherwise the performance counter), the
  GameTime, and n, the emission's ordinal among that GameTime's calls of the pool's alloc `0x470EB0`.
  Every live emitter calls the alloc exactly once a call, after its SFX test, whatever the pool
  answers, so an event recorder at the same point counts the same n with the pool emptied. Unit-script
  smoke, fire and wake are emitted only while the unit is in the viewed player's sight (`0x480EC5`), so
  emission depends on the viewer, as it does in stock. That makes the simulation step's emissions a
  function of the game and the viewer on either renderer. The flying pieces' smoke and fire are not:
  the explosion DRAW emits them (`0x421583`, `0x4215AA`), once a frame that ran ticks, so whether they
  exist, and n for every emission after them in their GameTime, also follow the frames drawn.
  **For the GPU path** this changes nothing: those emissions are made by the CPU at draw time and
  recorded as events like the others, with the seed the CPU gave them, so the GPU reproduces whatever
  the CPU emitted. Only the claim "the same game on GDI and on Vulkan shows the same particles" is
  weaker for them, as it is in stock.
- **The particle** is the object's count of the particles it began (nano 0..4 then 5..9, wake 0 and 1,
  flare 0..3). A smoke puff's is its spawn ordinal, whose low 8 (Smoke2) or 10 (Smoke1) bits the puff
  carries in its position's low bits; an update recovers it from those bits and the object's spawn
  count. Its frame count is draw 0 and each frame advance's countdown draw is the frame it advances
  to. Draws an emitter makes before its object exists use the particle `0xFFFFFFFF`.
- **Where it lives:** on GDI and in the engine's code on Vulkan, a side table by pool slot, particle
  state the simulation never reads. On Vulkan's GPU path, the emitter event will carry the seed.
- **This is a simulation change:** start positions and meteors move compared with stock for a given
  seed. It is a `SIM_FEATURE` (`particle_rand`) and is landed through the replay gate.

### The emitter event (Vulkan)

On a Vulkan launch each emitter entry records `{class, layer, start tick, end tick, seed, parameters}`
into a game-thread event list instead of allocating an object:

- **parameters:** point, box, from/to, period, life, sequence choice, frame cap and countdown, as the
  emitter's arguments and init give them;
- **end tick:** computed at emission from the same rule the GPU uses, including smoke's frame
  countdowns and the wake's death over shallow ground, which is found once on the CPU from the
  terrain's height `0x485070`;
- **endless feature smoke (`0x472C50`)** lives until the level's teardown or the layer cap trims it,
  as in the engine.

The list keeps the engine's layer cap (front trimmed when a layer passes its cap), so a layer never
holds more than the engine's would. It is published in the frame packet. The list is presentation
state; a seek clears it, as it clears the layers today.

### The draw (Vulkan)

One instanced draw per layer group. An instance is (event, particle index). Its vertex shader computes:

- **Position:** start + step × ticks for fire, flare, nano and wake. Smoke adds the wind's prefix sum
  between birth and now (a small per-tick table, uploaded as it grows) and its rise.
- **Frame or colour:** periodic in ticks for fire, flare, nano and wake. Smoke's frame advances by the
  random rule's countdowns, at most the sequence's frames per puff.
- **Life:** born at the spawn's tick (nano's second burst at T+1, flare particles at T+10/20/30,
  smoke's repeat every `interval`); dead past its end tick or below the wake's death tick. A dead
  instance emits a degenerate quad.

Sprites use the existing effects atlas and program. Dots stay 2×2 game pixels. The line-of-sight test
becomes the engine's own tile rule, the exact tile `(hi(x)>>5, (hi(y) − hi(alt)/2)>>5)` against the
viewed player's sight or mapped bit. Smoke1 is ungated as in the engine. That replaces today's smoothed
fog sample, whose edges differ from the engine's. The layer order and depth keys stay those of
`tagpu_sfx.c`.

### The pool (Vulkan)

The pool's capacity operand `0x471C83` is written 0 at attach on a Vulkan launch, in both builds. The
stock build has no `lim_dword`, so it needs its own write. Any emitter call that reaches the engine
anyway makes no particle. The walker's call `0x4955BF` and the particle part of the frame packet are
removed on Vulkan once the last class is drawn from events.

### The oracle

`tagpu_particlecheck.on` keeps the engine's pool and walker running on a Vulkan launch, under the
random rule. Each tick it compares every live sub-particle's position, frame or colour and death with
the closed form, as the repose oracle compares vertices, and logs any difference by class and site. A
GDI game of the same scenario at zoom 1.0 remains the picture's ground truth.

### Fixed on both renderers

The engine divides by zero for a teleport shorter than 5 world units (`0x47378E`) and for bubbles
emitted exactly at sea level (`0x474811`). Both become no particle, on GDI as an engine fix and on
Vulkan in the event.

## The landings

0. **The scout's facts into the engine map, the notes corrected.**
   - `effects.md` §7: `0x472630` makes Smoke2 ("smoke 1"), not a flare; weapon smoke is layer 9 and
     layer 5 is burning-feature smoke and the teleport flare; Smoke1 re-puffs every 5 ticks for ever;
     `0x4729D0` takes a layer; both smoke classes use "smoke 1" except BlackSmoke.
   - `engine/weapons.md` and `engine/sim.md`: the 61 `rand` sites classified, the loader-thread order.
   - The recorder's comment that calls `0x472630` a flare.
   - Measure each emitter's calls per tick in the battle from the recorder's sink counts, to size the
     event list and the instance buffer.
1. **The random rule on both renderers** (simulation). *Landed locally 2026-11-22.* The particle `rand` sites go to `particle_rand`,
   with the seed side table on GDI, and the two engine divide-by-zero fixes land. Checks:
   - the replay gate;
   - meteors and start positions no longer depend on particles: the same seed with the particle code
     disabled through the console `SFX` toggle `0x51E608` gives the same strike points and starts;
   - a GDI A/B for the look.
2. **The event list, the GPU path, and the nano spray.** *Landed locally 2026-11-22.* The emitter
   events, the instanced draw and the oracle lever, for the nano class first. It is the commonest
   particle in a battle, already recorded, and its closed form is the simplest. The engine's pool still
   runs for every other class. Check: the oracle reports 0 differences over a battle; a GDI A/B.
   - **What it built** ([effects](effects.md) §7.8): the event `inc/tagpu_pevt.h` (80 B: seed, T, layer
     and flags, the init's two cut boxes, ten step counts) and its closed form, written once in C and
     ported statement for statement to the vertex stage; the step count computed at emission with the
     spawn's own x87 sequence, the one inexact operation, so the GPU takes no square root; the list on
     the game thread with the layer's cap, purged after each walk, cleared at a seek; the packet's event
     table; one draw of 60 vertices an event, no vertex input; the engine's exact tile sight test from
     the fog source; `tagpu_particlecheck.on`.
   - **Moved to landing 5**: the wind table. Nano has no wind; smoke is the first class that needs it.
   - **Learned**: the emission must keep counting n where the pool alloc is (`prand_alloc`), so the
     event is recorded inside the alloc's stub, which skips the pool for a nano caller; an emitter
     whose pool answers NULL skips its init, trim and append with no other trace (engine map,
     *The emitters' alloc and their frame*). An event re-fired between two steps (a replay's frame
     visit) is `LATE`: its first update is a tick later and its second burst always exists.
   - **MEASURED**: the oracle in the 13 500-unit battle, GameTime 1 to 3 622, 159 512 073
     sub-particles, 0 differing, 0 objects missing or extra. Across a live rewind, 1 783 ticks, 0
     differing. The `battle` replay test group on the coverage build: 131 pass, 0 fail. A GDI pair on
     `nano-factory` at 1×: the spray at the same places on both.
     The battle at 1080p, two runs each: `particles` 3.46/3.92 → 2.01/2.01 ms a frame, `publish`
     2.82/3.03 → 2.47/2.49, `gather` 5.94/5.52 → 5.70/5.62 ([effects](effects.md) §7.8).
3. **Fire and the flare.** *Landed locally 2026-11-22.* Check: the oracle; a teleport and a
   burning-debris scene against GDI.
   - **What it built** ([effects](effects.md) §7.8): the event's class field and its sprite
     layout, `tagpu_pevt_sprite_at`/`_finish` in C and GLSL; the flare sequence's frames in the
     packet, put through the effects atlas by the render thread into a 64-row table the vertex
     stage indexes; one program for the three classes (the class bits pick the closed form); the
     oracle extended to fire and the flare, pairing by seed.
   - **The layer cap, exact by construction: a proxy object.** Fire and the flare share layers 5, 7
     and 9 with classes still drawn from the engine's objects, and the engine trims the oldest of a
     layer whatever its class. So these two classes keep their pool object: the class's init
     (`0x4742C0`, `0x4736E0`) is hooked and turns it into a proxy with no particle that the emitter
     trims and appends as stock, the walker destroys at the walk of D (its slot 3), and whose
     destroy removes its event. The layer's count and order, the pool's count and the stock build's
     pool fullness are the engine's. Landing 6, which empties the pool, must move them to nano's
     no-object path, with event-only counting, once no engine class is left in their layers.
   - **Learned**: the flare makes its second particle with 9 steps, not 10 (the walk of T + 10
     destroys the object only when particle 0, removed by the update past T + steps, is gone by the
     walk of T + 9). The pieces' fire is emitted from the explosion draw, after its tick's walk:
     `LATE` covers it with no special case. COB fire in stock content is the Atlas's and the
     Valkyrie's `ProcessFlames` (`EMIT_SFX` 0); the 13 500-unit battle has none.
   - **MEASURED**: the oracle in the 13 500-unit battle, GameTime 1 to 2 885: 1 066 702 fire and
     118 192 278 nano particles, 0 differing, 0 missing, extra or trimmed; `nano-factory` with eight
     Atlases flying, across a live rewind, 3 139 ticks: 15 522 fire (COB, layer 7) and 466 122 nano
     particles, 0 differing; `replay-teleport`, eighteen teleports of four riders, 3 684 ticks:
     35 008 flare particles, 0 differing. `tareplay.py run battle teleport --cover` passes (184 checks). Against GDI, fire and the flare
     at GDI's places and frames, in the restored twin's colour as before. Battle13k, two runs each:
     particles 2.27/1.84 → 1.99/2.10 ms, publish 2.55/2.35 → 2.36/2.41, gather median 4.39/3.78 →
     4.76/4.74 (open: noise or a cost, below).
4. **Wake and bubbles,** with the death tick from the terrain at emission. Check: the oracle; ships on a
   shore and submarines against GDI.
5. **Smoke1 and Smoke2,** with the wind table and the countdowns. Check: the oracle; feature smoke, impact
   smoke, rocket trails and damaged-unit smoke against GDI.
6. **The engine's particles off on Vulkan.** The pool capacity 0, the walker's call and the packet's
   particle copy removed on Vulkan, and `tagpu_sfx.c`'s gather of the layers retired. Measure in the
   battle at 1080p and at 4K live: the `particles` span, `publish`, `gather`, the GPU's frame time, and
   the process's address space and GPU memory.

Each landing is a local landing on this branch. The feature lands on main once, through the replay gate.

## Open

- **The layer cap's count of events is exact only while nano shares its layer with nothing.** Every
  caller files nano in layer 6 and no other emitter does. Fire and the flare, which share layers 5, 7
  and 9 with smoke and bubbles, keep a proxy object in the engine's layer since landing 3, so the
  engine counts and trims them itself; landings 4 and 5 can give wake, bubbles and smoke the same
  proxy, and landing 6 retires the proxies with the pool.
- **The shared pool no longer holds nano on Vulkan.** The stock build's pool of 1 000 objects counted
  nano with every other class; with nano as events, a pool that was full in stock has room for
  another class's object, and a nano emission is never refused for a full pool. Presentation only
  (no simulation value reads a particle since landing 1), and gone once every class is an event.
  Fire's and the flare's proxies take their pool slots as the real objects did.
- **The gather's median rose 0.4 to 1 ms in landing 3's battle13k runs** (4.39/3.78 → 4.76/4.74);
  the base's own two runs differ by 0.6 ms, and the landing's additions to the gather (64 frame
  lookups, the event copy) are microseconds. A third pair, or a timer around `tagpu_fx_set_events`,
  settles it.
- **The blend order inside a layer run** is the events' emission order after every gathered object
  of the run (0..6 or 7..9), not the engine's layer-then-vector order: a fire sprite draws over the
  layer-9 smoke beside it whatever their ages ([effects](effects.md) §7.8). It goes when smoke is
  an event too (landing 5), if the events are then drawn in layer order.
- **An event is culled by its box, not each particle by its own**: one particle's quad can be
  computed for nothing, never one missed (the box holds both ends and a step's slack).
- **A frame whose packet carries no fog source** (a copy that failed) draws the event dots without the
  tile test, off-map ones included, where the units fail closed; the engine tests every dot. Every other
  frame takes the engine's rule exactly.

- **The emitter rates, measured** (2026-11-22). The 13 500-unit battle (`tools/battle13k.py`'s game:
  Total Mayhem on Zombie Kingdom, 1920x1080, Vulkan, `--defaults`, a private X display, this branch's
  DLL), the live game viewed by player 0, about 30 ticks a second. The recorder's counts (the ten
  sinks and the two nano roots, `tagpu_recorder.c` *THE COUNTS*) were read from 100 dumps asked 1.5 s
  apart through `tagpu_replay.trigger`, GameTime 67 to 4483, windows of 38 to 47 ticks (median 45).
  **No counter gives a single tick**: the peak below is the highest 45-tick window's mean. 13 500
  units at the start, about 6 900 alive at GameTime 3 600.

  | Emitter | Calls | Mean a tick | Peak window a tick |
  |---|---|---|---|
  | wake `0x472430` (`EMIT_SFX` 3 and 5, 510 352 each) | 1 060 538 | 240 | 315 |
  | nano `0x4720D0` (436 950 from `0x402AC2`) | 820 728 | 186 | 607 |
  | grey smoke `0x472810` (flying pieces 325 537, trails `0x49BDC1` 180 775, COB 37 092) | 543 420 | 123 | 372 |
  | fire `0x472AB0` (the flying pieces) | 306 572 | 69 | 151 |
  | explosion and corpse smoke `0x472630` | 101 940 | 23 | 79 |
  | black smoke `0x4728F0` | 89 939 | 20 | 36 |
  | shot trail `0x4729D0` | 37 369 | 8.5 | 62 |
  | nano reverse `0x472200` | 11 994 | 2.7 | 12 |
  | bubbles `0x472530` | 373 | 0.08 | 0.5 |
  | COB fire `0x472330`, teleport `0x471FD0`, `SmokeInfinite 0x472C50` | 0 | 0 | 0 |
  | **all** | 2 972 873 | 673 | 1 263 (GameTime 144–187, the opening volley) |

  `SmokeInfinite`'s 55 objects come from the map's load, before the counts start. **Live objects**,
  the `sfx:` line's layer counts (`tagpu_sfx.on=log`, every 60 frames, 47 samples): layer 2 (wake)
  median 13 013, most 16 118; layer 6 (nano) 4 245, 8 217; layer 9 6 195, 15 888; layer 4 55; layer 7
  at most 18; **all layers median 21 692, most 35 903**. Sub-particles were not counted: the frame
  packet's 24 576 particle entries were full in 1 665 of 1 712 gathers (the packet line's
  `part=24575/24575 thin=1665`), so today's native draw thins through nearly the whole battle and the
  live total is above 24 576. The bound from the classes ([effects](effects.md) §7.3): two dots a wake object,
  ten a nano object, one to four a layer-9 object — about 32 000 + 82 000 + 64 000 at the sampled
  maxima, which do not coincide. So the event list holds about 36 000 live events at the most seen and
  takes up to about 1 300 new ones a tick; the instance buffer is sized in sub-particles, past 24 576.
  A full buffer thins as `tagpu_sfx.c` does today and says so, never silently.
- What the network start shuffle's order at `main+0x29FC` decides is not established. It draws the game
  thread's stream, which carries no particle draw since landing 1; the two-player game
  (`recmp_create_begin_test.py`) passes on both builds with the rule in (2026-11-22).
- Whether any saver dumps the main block raw is not established. The layer-table pointer would be a
  dead value there. A save and a load with particles alive play on, on Vulkan and on GDI
  (2026-11-22, `sfx-strait`): the loaded level makes its particles anew.
