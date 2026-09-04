# JSON scenarios — a safe, text-based situation format for agent testing

*Outcome of the `/grill-me` interview, 2026-09-01. Goal: an agent drops one JSON file and
one CLI command, and lands in a **specific, reproducible in-game situation** — 200 units
fighting, a wreck of a chosen type on screen, the camera already pointing at it. Companion
references: `tacli-design.md` (the launcher/driver this extends), `gui-gadgets.md` (the
`ui` layer that replaces menu navigation), `cmdline-options.md` (the INI/registry knobs),
`file-formats.md` (asset formats), `runtime-injection.md` (hooking).*

Sources tagged `[VERIFIED]` were read this session from the vendored TADR corpus
(`vendor/TADR/src/DDraw/*`, `vendor/TADR/src/Recorder/TAMem/*`) or from this project's own
verified notes. `[CLAIMED]` = community assertion not yet A/B'd on a live instance.

---

## The one-paragraph summary

A scenario is a **strict JSON file describing a situation**, not a savegame. `tacli
scenario load` launches an instance, drives the shell menus by gadget name, waits for the
game, and then the fork **spawns the described units and features by calling the engine's
own creation functions** — the same ones TA itself uses. Nothing is ever addressed by
numeric id: units, features, orders and engine switches are all named, and the names are
resolved against the *live* game, so a mod that swaps what index 42 means cannot corrupt a
scenario. Validation happens three times (schema, catalogue, in-process) and the applier
**creates nothing at all** unless every entity resolves.

## Why not TA's own save format

TA's `.sav` is binary and index-keyed: it is fragile against exactly the mod-swap case
this format exists to survive, has never been fully reverse-engineered, and cannot express
"apply this on top of a game that is already running". Writing a JSON→`.sav` compiler
would mean re-deriving a format where every wrong field is a crash or silent corruption.
Runtime spawning rides machinery this project already owns and composes: a scenario is a
set of mutations, so an agent can stack "now add 50 more bombers" onto a live fight.

**Prior art that settles feasibility**: `vendor/TADR/src/DDraw/MultiplayerSchemaUnits.cpp`
already spawns units at runtime from a text schema in skirmish and multiplayer, resolving
unit names by scanning the live definition table. This design follows its recipe. [VERIFIED]

---

## Locked decisions

| Branch | Decision |
|---|---|
| Mechanism | **Runtime spawn from inside the fork**, calling the engine's own `UNITS_CreateUnit` / `SpawnFeatureOnMap` / `Order2Unit`. Not a `.sav` writer, not a generated `.ota` mission. |
| Menus | **No menu navigation by the agent**: `scenario load` drives `ui click SINGLE → Skirmish → Start` internally, each step auto-waiting and reporting the screen it landed on. Not an engine bypass — the Start button *is* map load, player setup and start positions, so it gets pressed rather than reimplemented. |
| File ownership | **Self-contained.** `setup` carries map, players, resolution, unit limit, switches; the entity list carries the situation. `load` = launch + apply (always a clean start); `apply` = mutate a live game, ignoring `setup`. Two verbs, two unambiguous meanings. |
| Scope | Units (type, owner, position, facing, health, nanoframe, stance), features/wrecks, initial orders, player resources, camera, engine switches. **Out of v1**: projectiles, smoke, build queues, AI internal state, terrain edits — and they are a **schema error**, not a silent no-op. |
| Coordinates | **World units, one space**, exactly what `roster` / `eye` / `click` already speak: `pos: [x, y]` on the eye plane, optional `height` (default: engine snaps to terrain). No normalized space, no tile space. Out-of-map is a validation error. |
| Facing | **Degrees, `0` = TA's own default build facing** (heading word `0x8000`, *not* `0x0000`). Omitted = engine default. |
| Bulk authoring | **`groups` (count + composition + pattern) and a flat `units` array, both.** Expansion is **seeded and deterministic**, and happens in Python — the DLL only ever sees a flat list. `scenario expand` prints it without launching. |
| Transport | **CLI compiles JSON → a private, versioned, line-oriented wire format**; the DLL scans that. The DLL never parses JSON *in* (it does write JSON *out*). This is the safety property: one testable component decides what reaches the engine. |
| Identity | **Author-chosen string handles** → ordinals at compile time → `UnitStruct*` in a spawn table at apply time. Engine indices are never the public identity (`UnitInGameIndex` is recycled on death). |
| Validation | **Three layers**: strict schema (unknown keys are errors), catalogue check against the live unit/feature/map lists, and an in-process **resolve-before-create** pass that creates nothing if anything fails. `on_error: "skip"` opts into best-effort, and it acts in **layer 3**: layer 2 downgrades to a warning and passes the name through, because only the fork can drop entities one at a time. Layer 1 stays absolute — *not a name* is not *not in this game*. |
| Timing | **Detect** the trigger in the present path (existing idiom); **apply** from a detour at **`0x4969D2`** — five position-independent bytes inside the block `0x4969CB` (TADR's `GameTickHook` address) enters — never mid-render, where the sort-grid walk lives. All entities in **one visit**, so the situation is reproducible. |
| Existing units | `setup.clear_existing` defaults **true**, removing the skirmish's starting commanders silently via `UNITS_KillUnit(u, 0)` — a scenario contains exactly what the file says. It runs over a **snapshot** taken first, and **before** the create pass, for reasons phase C measured (below). |
| Camera | `at` and `center_on` both mean **the centre of the window**, never the eye origin. Group targets compile to a coordinate; entity handles resolve to the unit's *actual* post-snap position. `pin` defaults false. |
| Orders | The engine's own order **names**, compiled to TA's order constants and resolved per unit through **`ScriptAction_Type2Index`** (TADR's own path). TA has **no attack-move**; the idiom for a meeting engagement is `attack` a ground position. |
| Switches | `setup.switches` name-keyed to the `SoftwareDebugMode` bits. **`shootall` defaults on** (the universal player convention); always echoed in the result. |
| Assertions | **None.** The loader guarantees the setup and reports requested-vs-actual per entity; judging the outcome is the agent's job with `roster` / `shot` / `glshot`. |
| Round trip | The schema is **designed to be dumpable** (every field readable back out of the engine); `scenario dump` is the first follow-on, not a v1 gate. |
| Tests | The compiler is pure Python and gets tests in `tools/test_tacli.py` — the harness that already caught a real bug on its first day. |

---

## The file

```json
{
  "format": "ta-scenario/1",
  "description": "200 ARM vs 200 CORE meeting engagement on the Two Continents plateau",
  "seed": 20260901,
  "on_error": "abort",

  "setup": {
    "map": "Two Continents",
    "res": "1024x768",
    "unit_limit": 500,
    "clear_existing": true,
    "switches": {"shootall": true, "noshake": true},
    "players": [
      {"slot": 0, "controller": "human", "side": "arm",  "color": 0},
      {"slot": 1, "controller": "ai",    "side": "core", "color": 1}
    ]
  },

  "groups": [
    { "id": "arm_wave", "owner": 0, "at": [2200, 1200],
      "pattern": {"kind": "grid", "cols": 20, "spacing": 40, "jitter": 6},
      "composition": [{"type": "ARMPW", "count": 150}, {"type": "ARMROCK", "count": 50}],
      "facing": 90,
      "orders": [{"cmd": "attack", "to": [3800, 1200]}] },

    { "id": "core_wave", "owner": 1, "at": [3800, 1200],
      "pattern": {"kind": "grid", "cols": 20, "spacing": 40, "jitter": 6},
      "composition": [{"type": "CORAK", "count": 200}],
      "facing": 270,
      "orders": [{"cmd": "attack", "to": [2200, 1200]}] }
  ],

  "units": [
    { "id": "hero", "type": "ARMCOM", "owner": 0, "pos": [2000, 1200],
      "facing": 90, "health": 60, "stance": "hold" }
  ],

  "features": [
    { "id": "the_wreck", "type": "armlab_dead", "pos": [3000, 1200], "facing": 45 }
  ],

  "camera": { "center_on": "the_wreck" }
}
```

**Rules that are not obvious from the example**

- **Unknown keys are a hard error.** These files are written by agents; a typo'd `"unit"`
  for `"units"` under a permissive schema yields an empty map and a lost afternoon.
- `seed` is optional and defaults to a hash of the file content, so an unseeded scenario is
  still deterministic. `expand` reports the seed it used.
- `health` is a **percentage** (mirrors `HealthPerA`); `nanoframe` is separate, for a
  half-built look, and is the percentage **built** — `40` is a 40%-complete scaffold.
  The engine's own field is the fraction *remaining*, so the applier inverts it.
- `stance` is a token (`hold` / `manoeuvre` / `roam`), never a raw mask.
- `orders` at group level apply to every member; a unit-level `orders` overrides. Targets
  are a coordinate (`to`) or a handle (`target`) — a unit **or a feature** (that is how a
  wreck gets reclaimed), never a group.
- Every per-entity attribute (`facing`, `height`, `health`, `nanoframe`, `stance`,
  `orders`) may sit on a group, where it applies to every member.
- **`at` is the *centre* of a formation**, not its corner — the same thing `at` means for
  the camera, so there is one rule for the word.
- Patterns in v1: `grid`, `line`, `random` (in a rect). All seeded. A `line`'s optional
  `angle` is a **layout** direction in world degrees (`0` = +x, `90` = +y) and is
  deliberately *not* `facing`: laying units out and pointing them are two questions.
- **`null` means "unset — use the engine's default"** wherever a value may appear.
- `owner` and player `slot` use the **same numbering as `tacli --player`** — no third
  convention, and phase C measured what that numbering is: TA's own registry keys are
  `Player0Controller`..`Player9Controller` and the roster reports `own=0` for the first
  of them, so a slot **is** the engine's 0-based `Players[]` index, `0..9`. (Phase A
  bounded it `1..10` and so could not name the human seat a plain launch uses.)
  When `setup.players` is present, an `owner` outside it is an error.
- Handles are `[A-Za-z0-9][A-Za-z0-9_.-]{0,31}`. An entity with no `id` gets a synthesized
  one containing `#`, which an author's cannot — so a file that names nothing is still
  reported entity by entity, with no chance of collision. Group members are
  `<group>#<n>`.
- Aliases accepted: `guard` → TA's `defend`; `maneuver` → `manoeuvre`.
- **Type names are matched case-insensitively and emitted in the game's own
  spelling.** TA's two tables disagree — units are `ARMPW`, wrecks are
  `armlab_dead` — so with a catalogue the file may write either and the wire carries
  what the engine calls it; with no catalogue the author's spelling is passed through
  untouched, because inventing a case would be inventing a name.
- Per-player unit counts are checked after expansion: over `setup.unit_limit` is an
  **error**, over TA's stock cap of 250 with no limit set is a **warning** (the engine
  would silently drop the rest).

## Order vocabulary

TA's own order names, from the engine's button/order table [VERIFIED,
`TA_MemoryStructures.pas:6-21`]:

`stop`(1) `move`(2) `attack`(3) `blast`(4) `unload`(5) `load`(6) `defend`(7) `repair`(8)
`patrol`(9) `reclaim`(12) `capture`(13) `mobilebuild`(14)

**There is no attack-move in Total Annihilation.** Units *do* fire while moving, but a
`move` order alone will not make a blob seek the enemy. Three idioms, all supported:

| Idiom | Shape | Behaviour |
|---|---|---|
| **`attack` a ground position** (recommended) | `{"cmd": "attack", "to": [x, y]}` | Units advance on the spot and engage what they meet. Closest thing to attack-move. |
| `attack` a handle | `{"cmd": "attack", "target": "enemy_king"}` | Precise and reproducible; they beeline, and stop when it dies. |
| `move` + `stance: "roam"` | | Most emergent, least explicit. |

A scenario asking for `"attack-move"` is rejected at compile time with *"no such order; TA
has none — use attack with a coordinate"*.

## Engine switches

`SoftwareDebugMode`, a `u16` at `TAdynmemStruct + 0x37F2F` [VERIFIED, `tamem.h:598`],
cross-checked by this project's own work: `0x37F2F + 2 = 0x37F31`, already verified live as
the `-t` network timeout (`cmdline-options.md`). Readable today with no new code:

```bash
tools/tacli peek t1 '*0x511DE8+0x37F2F:2'
```

Bits [VERIFIED, `tamem.h:1862-1876`]:

| Name | Bit | Note |
|---|---|---|
| `drop` | `0x1` | |
| `cheats` | `0x2` | `CheatsEnabled` — try this first if a `+` command appears inert |
| `selboxes` | `0x4` | |
| `invulnfeatures` | `0x8` | |
| `noshake` | `0x10` | **Screen shake off — valuable for `ta-capture` frame comparison** |
| `clock` | `0x40` | |
| `doubleshot` | `0x80` | |
| `halfshot` | `0x100` | |
| `radar` | `0x200` | |
| `shootall` | `0x400` | **Default on.** The universal player command. |

`+shootall` is a **stock TA chat command**, not a TADR invention: TADR's share dialog emits
the literal string (`sharedialog.cpp:247`) and its preset button sends
`+setshareenergy 1000\r+setsharemetal 1000\r+shareall\r+shootall` (`dialog.cpp:129`).
`KeyboardHook.pas:144` toggles the **memory bit directly**, which is why a two-byte write is
an equivalent path with no chat plumbing. [VERIFIED]

Its precise effect is a measurement now, not lore. **[VERIFIED live, phase D]**
`scenarios/shootall-ab.json` is the A/B: six ARMPW on `hold` stance so they cannot walk
anywhere, an enemy CORSOLAR 130 world units away — in range, and unarmed, so it cannot
shoot back — and a second CORSOLAR far out of range so its owner still owns something and
the game keeps ticking (a player with no units is a defeat, and the applier stops). One
game, one bit, `tacli switches` between the halves:

| `shootall` | 45 seconds later |
|---|---|
| off (`0x001C`) | the Solar Collector stands, full health bar, `alive=8` |
| on (`0x041C`) | `Wreckage M:116` where it stood, `alive=7` — and every Peewee still on its spawn coordinate |

So **idle units engage an enemy *building* in range only with the bit set**: the community
meaning, confirmed, and the reason `shootall` defaults on here. `cheats` was not needed.
The engine's read site is still unfound, but the behaviour no longer waits on it.

Also exposed as a live verb on any instance, scenario or not:

```bash
tools/tacli switches t1 shootall=on noshake=on
```

---

## The engine recipe

Every call is `__stdcall` and every address is from the merged community symbol corpus
(`tools/ta_symbols.txt`, provenance in `ta_symbols_sources.tsv`).

| Step | Call / field | Address | Notes |
|---|---|---|---|
| name → type index | scan `taPtr->UnitDef[i].UnitName` over `UNITINFOCount` | — | What TADR's spawner does. `UNITINFO_Name2ID 0x488B10` exists but the scan is the proven path and needs no call. Case-insensitive here, so the wire may carry either spelling. |
| create a unit | `UNITS_CreateUnit(owner, typeIdx, x, height, y, fullHp, stateMask, unitNumber)` → `UnitStruct*` | `0x485F50` | Positions are **16.16 fixed point** in the 3-D convention `(x, altitude, depth)`. `unitNumber = 0` lets the engine allocate; `fullHp = 1`, `stateMask = 1`, exactly TADR's call. [VERIFIED live] |
| snap to ground | read `FeatureMap[(x>>20) + (z>>20)*FeatureMapSizeX].height` | `main+0x14287` | TADR's own height rule; feature cells are **16 world units**, `FeatureStruct` stride `0x0D`, `height` at `+0x04`. |
| feature name → id | `FeatureName2ID(name)`, then `LoadFeature(name)` if it is `-1` | `0x422DD0`, `0x4224B0` | The two-step is TADR's `TAMap.PlaceFeatureOnMap`: a feature the map never loaded is loaded on demand. |
| feature grid cell | `GetGridPosPLOT(x/16, z/16)` → `PlotGrid*` | `0x481550` | `SpawnFeatureOnMap` wants the cell, not the coordinate. |
| place a feature | `SpawnFeatureOnMap(gridPlot, defIdx, position, volume, playerId)` | `0x423C50` | Silent and instant. `position` is 16.16 `(x, altitude, depth)`; `volume` is the `{bank, pitch, heading}` word triple; `playerId = 10` is what TADR passes for a map feature. [VERIFIED live] |
| order name → script index | `ScriptAction_Type2Index(&idx, orderType, unit, target, pos)` → `char*` | `0x43F0E0` | **Not** `ScriptAction_Name2Index`. TADR's `SendOrder` resolves the *per-unit* script index from the order type, the unit and its target; the returned `char*` points at the byte to pass on, and `NULL` means this unit cannot take that order. Its `ScriptAction_Index2Handler` (`0x438830`) call is dead — that function is a pure `base + *ecx*25` address computation whose result TADR discards. |
| issue an order | `ORDERS_NewMainOrder2Unit(*scriptIdx, shift, unit, target, position, 0, 0)` | `0x43AFC0` | `position` is **whole world units** in the screen convention `(x, depth, altitude)` — *not* 16.16, and transposed against the create call. See the asymmetry note below. [VERIFIED live] |
| remove a unit silently | `UNITS_KillUnit(unit, 0)` | `0x4864B0` | Mode `0` = the "recreate proc" path (no explosion). Mode `3` is a normal death **with** wreckage. Park `ActiveCommanderDeath` at 0 across the sweep — see below. |
| commander-death gate | `ActiveCommanderDeath` | `main+0x37EF6` | `0x486688` compares it against zero and only then calls `UNITS_KillAllForPlayer`. [VERIFIED, binary] |
| apply point | `Game_MainLoopTick` detour | `0x4969D2` | See *The apply point* below. |
| map extents | `MapWidth/Height` | `main+0x14223`/`0x14227` | World units. Bounds-checking source; `FeatureMapSizeX/Y` (`0x14233`/`0x14237`) is the same map in tiles. |
| per-player cap | `MaxUnitNumberPerPlayer` | `main+0x37EEC` | Reads **250** in stock skirmish, and **500** after `setup.unit_limit: 500` (phase D writes it to `totala.ini`; the array grows with it, `array_slots` 2500 -> 5000). `ActualUnitLimit` (`0x37EEA`) reads 0 either way and is not written. |
| loaded map | `GameingState.TNTFile` | `*(main+0x391E9) + 0x204` | The map the engine really has, `"Maps\Two Continents.TNT"`. TA falls back silently on a `SkirmishMap` it does not know, so this is the only honest answer [VERIFIED live, tamem.h:632,811]. |
| player resources | `PlayerStruct[10]`, stride `0x14B` | `main+0x1B63` | `fCurrentEnergy +0x8C`, `fCurrentMetal +0x98`, `fMaxEnergyStorage +0xA4`, `fMaxMetalStorage +0xA8` — all confirmed against a live read, and the stride with them (slot 1 at `+0x1CAE`). Writable, but not *settable* from the apply point: see the phase C notes. Set them **at launch** instead, `Player<N>Metal`/`Energy` in the skirmish registry key (phase D). |

### The apply point

The note originally fixed this at `0x4969CB`, TADR's `GameTickHook` address. That is a
`push ebx; push ebx; call 0x468CF0` — seven bytes over three instructions, the last of
them **eip-relative**, so a stolen-bytes trampoline would have to re-encode the call.
Four bytes later, `0x4969D2` holds `A1 E8 1D 51 00` (`mov eax, ds:0x511DE8`): exactly five
position-independent bytes, an E9 fits with no NOP pad and nothing to relocate, and it is
straight-line code from `0x4969CB`, so it fires exactly as often. Incoming EFLAGS are dead
at the resume point (`lea`/`call`/`mov`/`mov`, and the first flag consumer at `0x4969E9`
writes them first), so `pushad`/`popad` alone bracket the call — the same argument
`tagpu_tracer.c` makes for its two sites. Two nearby branches jump to `0x4969CB` itself,
never into the middle of the patch.

TADR's own comment (`AreaDamageOverflow.cpp:481`) says this site fires **9-15x per
simulation tick**, not once, and sits *outside* the sim loop. The applier therefore gates
on a one-way state machine (`IDLE → ARMED → APPLYING → DONE`) with an interlocked
hand-off, not on a call count — and "one tick" in this design means **one visit**, which
is the stronger guarantee.

### The coordinate asymmetry

Creating and ordering do not speak the same language, and the two vendored corpora
disagree about the second one. `TA_MemUnits.pas` reads the order position's *high word*,
implying 16.16; TADR's C++ `ConstructionKickout` feeds `ORDERS_NewMainOrder2Unit` the
unit's whole-unit `XPos`/`YPos` words and divides `orders->Pos.X` by 16 to get a tile.
Phase C settled it by measurement rather than by choosing a source: the applier reads
`UnitOrders->Pos` (`+0x22`) back after issuing the first order and reports both numbers.
Live, `passed [3800, 1200, 84]` came back as `stored [3800, 1200, 84]` — **whole world
units, screen convention, stored verbatim**. TADR's C++ reading is correct.

**Unit fields written after creation** (`vendor/TADR/src/DDraw/tamem.h:986-1103`):

| Field | Offset | Note |
|---|---|---|
| heading | `+0x66` | `Volume_Word{bank, heading, pitch}` at `+0x64`. Full circle `0x10000`, quarter `0x4000`, **default `0x8000`**, increases CCW from above. |
| position | `+0x6A` X, `+0x6E` **altitude**, `+0x72` map depth | Three 16.16 dwords. **Naming trap**: tamem uses the screen convention (`XPos/ZPos/YPos`), Ghidra transposes it. Same bytes; do not "fix" either. |
| type index | `+0xA6` `UnitID` | The unit **type**, not the instance. |
| instance slot | `+0xA8` `UnitInGameIndex` | **Recycled on death — never a public identity.** |
| health % | `+0xF6` `HealthPerA`, `+0xF7` `HealthPerB` | Write both; the second lags the first and the GUI reads it. |
| owner slot | `+0xFF` `cOwnerID` | |
| build fraction | `+0x104` `Nanoframe` | Fraction **REMAINING**, `0.0` = finished (`build-state.md`). The file's `nanoframe` is the percentage **built**, so the applier writes `1 - n/100`. |
| health | `+0x108` | Max HP needs no `UnitDefStruct` offset: create with `fullHp = 1` and this field *is* the maximum, so scale it in place. |
| state mask | `+0x110` | `0x10000000` alive, and `(mask & 0xC0000) >> 18` is the stance: **0 hold, 1 manoeuvre, 2 roam** [VERIFIED, TADR `dialog.cpp:409-417`]. The applier also sets `0x20` for a nanoframe, which **the draw path does not read** — measured 2026-09-03 (G13l): under construction is `Nanoframe != 0` at `+0x104` and nothing else, and `0x20000000` is the **structure** bit, set by the engine for any building whether finished or not. A scenario nanoframe therefore renders correctly on the strength of `+0x104` alone, and the `0x20` write is inert. |

**Catalogue sources**: `UnitDefStruct` gives `UnitName` `+0x20`, `UnitDescription` `+0x40`,
`Side` `+0xA0`, `FootX/FootY` `+0x14A/0x14C` (so Python can warn when a group's `spacing` is
tighter than the units' footprint). `FeatureDefStruct` gives `Name` `+0x00`,
`Description`, `FootprintX/Z`, `Metal`, `Energy`; count at `main+0x14253`.

**Not available**: the FBI `Corpse=` value is inside `UnitDefStruct`'s unnamed
`data5` blob (`0xA8..0x13D`), so unit → its corpse cannot be mapped mechanically. Wrecks are
therefore placed by **feature name** in v1, discovered from `tacli features`. Finding that
offset is a clean v1.1 addition that would enable `{"type": "ARMCOM", "as": "wreck"}`.

`UNITS_CreateCorpse` (`0x4863D0`) is not a usable alternative: TADR only ever hooks it from
inside the death path, never calls it, so "make a corpse" really means "kill something" —
with the explosion, splash damage and settle delay that implies.

---

## Transport

Trigger files live next to the exe in the instance gamedir, so they are already
per-instance. Two established idioms, both followed:

- **In**: `tagpu_scenario.trigger` — a private, versioned, line-oriented text file the CLI
  writes and the DLL scans with a small fixed-vocabulary parser, then deletes
  (`tagpu_peek` idiom). No user hand-writes it; **no compatibility promise** — it changes
  freely whenever the DLL does.
- **Out**: `tagpu_scenario.json` — written **tmp + rename** so a reader never sees a
  half-built file, polled by the CLI in an auto-wait loop (`tagpu_ui` idiom,
  `inc/tagpu_ui.h`).

As emitted by `scenario expand --wire` (phase A). Blank lines and `#` comments are
skipped; `-` is "unset, use the engine's default"; columns are positional:

```
# tagpu scenario wire v1 — written by `tacli scenario`, read once by the fork.
v1 seed=20260901 clear=1 onerror=abort units=401 feats=1
map Two Continents
limit 500
sw noshake=1 shootall=1
player 1 metal=5000 energy=5000
# unit <ord> <type> <owner> <x> <y> <height> <facing> <hp%> <stance> <nano%>
unit 0 ARMPW 1 775 1098 - 90 - - -
feat 0 ARMCOM_DEAD 1700 1200 - 45
order 0 attack pos 2400 1200
cam feat 0 pin=0
end
```

Three details the sketch above did not have. The map gets **its own line** — the rest of
the line is the name — so a map with spaces needs no escaping. An order names its target
in a fixed vocabulary (`pos <x> <y>` / `unit <ord>` / `feat <ord>` / nothing, for `stop`),
which is cheaper to parse than guessing from the argument count. And **every entity line
precedes every order line**, because the fork creates the whole situation before issuing
anything — so an order can name a unit spawned later in the same tick. `end` closes the
file: a truncated one is visible rather than half-applied.

Ordinals are **per kind** (`unit 0` and `feat 0` coexist) and follow emission order: group
members first, in composition order, then the explicit `units`. Handle *strings* never
reach the engine — the fork keys its result by ordinal and the CLI maps it back.

The DLL re-validates every field regardless of what the CLI promised — bounds against the
live map extents, name lookups that can fail, a hard ceiling on entity count — because the
file is on disk and can be stale or truncated.

**Result JSON**, keyed by ordinal because that is all the fork knows; `tacli` puts the
handles back before an agent sees it:

```json
{"ok": 1, "frame": 630, "gametime": 279, "seed": 20260901,
 "requested": {"units": 401, "features": 1, "orders": 400},
 "applied": 402, "failed": 0, "cleared": 2,
 "orders": {"issued": 400, "failed": 0},
 "map": {"name": "Two Continents", "world": [10752, 12800], "tiles": [672, 800]},
 "limit": {"total": 0, "per_player": 250, "array_slots": 2500},
 "switches": {"before": 12, "after": 1052, "bits": {"shootall": true, "noshake": true}},
 "units": {"0": {"engine_index": 3, "owner": 0, "type": "ARMPW",
                 "requested": [1823, 1026], "actual": [1823, 1026, 85]}},
 "features": {"0": {"def": 87, "type": "armlab_dead",
                    "requested": [3000, 1200], "actual": [3000, 1200, 85]}},
 "order_probe": {"passed": [3800, 1200, 84], "stored": [3800, 1200, 84]},
 "camera": [3000, 1200], "pin": 0, "eye": [2616, 806], "errors": []}
```

`actual` carries three components — the two the file asked for plus the terrain snap the
engine chose. `order_probe` is the applier reading `UnitOrders->Pos` straight back after
the first order, so the coordinate convention is a measurement in every result rather than
a decision made once. `errors` is capped at 24 entries with an `errors_dropped` count, and
a failure that names no entity (a truncated file, a tick that never came) reports there
with `applied: 0`.

---

## CLI surface

Built today (phase A) — the three that need no game:

```bash
tools/tacli scenario list
tools/tacli scenario validate 200v200                  # schema + catalogue, no game
tools/tacli scenario expand  200v200 --json            # the flat entity list, no game
tools/tacli scenario expand  200v200 --wire            # the file the fork will read
```

Built in phase C, and needing a running **game** (not the menus):

```bash
tools/tacli scenario apply   t1 200v200 --json   # mutate a live game; ignores setup's launch half
tools/tacli switches t1                          # report the SoftwareDebugMode bits
tools/tacli switches t1 shootall=on noshake=on   # set them live, on any instance
```

`apply` checks names against the target instance's own cached catalogue by default
(`--instance`/`--catalogue` override it), writes the wire, polls for the result, and puts
the author's handles back over the fork's ordinals before anything is printed. With
`camera.pin` it also writes the eye-hold file, using the eye the fork actually wrote
rather than re-deriving the projection in a second place.

Built in phase D, and needing **nothing** — this is the one that starts from a stopped
instance, or from no instance at all:

```bash
tools/tacli scenario load  t1 200v200            # launch → menus → live → applied → camera
tools/tacli scenario load  t1 200v200 --restart  # ...on an instance that is already running
tools/tacli scenario load  t1 200v200 --res 1280x960 --map 'Comet Catcher'
```

`load` carries `setup`'s launch half — map, resolution, players, unit limit — into the
launch, drives `SINGLE → Skirmish → Start` by gadget name, waits for a world, reads back
the map the engine *actually* loaded, and only then applies. It is a clean start by
definition, so on a running instance it refuses unless `--restart` says otherwise, and
`--any-map` is the escape hatch for the map check. `launch --unit-limit` exposes the same
INI knob on its own.

Still waiting on the fork:

```bash
tools/tacli scenario dump    t1 -o /tmp/captured.json  # (phase E)
```

Built in phase B, and needing only a running instance:

```bash
tools/tacli units     t1 --json     # live unit catalogue -> the instance's cache
tools/tacli features  t1 --json     # live feature catalogue (wreck names)
tools/tacli maps      t1 --json     # SELMAP's MAPNAMES, read and put back
tools/tacli catalogue t1            # what this instance has cached
tools/tacli scenario validate 200v200 --instance t1    # layer 2 against that cache
```

Scenarios live in **`scenarios/` at the repo root**, tracked in git, plain `.json`. A bare
name resolves to `scenarios/<name>.json`; anything with a `/` or a `.json` suffix is a path.
Every verb takes `--json` and exits non-zero on failure. `setup` may carry launch options;
explicit CLI flags win, so a scenario re-runs at a different resolution without editing.
`validate` and `expand` take **`--instance <name>`** (that instance's cached catalogue)
or **`--catalogue <file>`** (a JSON `{"units": [...], "features": [...], "maps": [...]}`,
entries either names or objects with a `name` and a `footprint`). With neither, layer 2
does not run — which the human output says out loud rather than implying the names were
checked.

**Deliberately not added**: an inline `tacli spawn t1 ARMPW 1200,900`. It would be a second
path into the engine with its own validation story, and `scenario apply /tmp/one.json`
already covers it through the safe one.

---

## Phases and DoD

**A — the compiler (Python only). BUILT 2026-09-01.** Schema, strict validation, seeded
expansion, patterns, handle resolution, wire writer. `list` / `validate` / `expand`, all in
`tools/tacli` (one file, stdlib only, like the rest of it); `scenarios/200v200.json` is the
example above, shipped.
*DoD, met*: `expand 200v200 --json` yields 401 units + 1 feature byte-identical across
runs; unknown unit (with `--catalogue`), out-of-map coord — before *and* after expansion,
so a group shoved off the edge is caught too — duplicate handle, unknown key and
`"attack-move"` each produce one clear error naming the exact path that is wrong;
`python3 tools/test_tacli.py` passes (140 tests, no wine, no game, 0.02s).

Determinism is stronger than the DoD asked: expansion draws from a **six-line xorshift32**
rather than `random`, because an expansion is a published artifact — two runs, two
machines, months apart — and the stdlib makes no promise about its stream. An unseeded
file hashes its own content *minus the seed field*, so reformatting does not reshuffle the
jitter.

**B — catalogues (fork + CLI). BUILT 2026-09-01.** `tagpu_cat.c` walks both definition
tables on demand (`tagpu_units.trigger` → `tagpu_units.json`, the same for features), and
`tacli units` / `features` / `maps` fold the answers into a per-instance
`catalogue.json` that `scenario validate|expand --instance <name>` checks against.
*DoD, met — and one line of it was wrong*: stock TA lists `ARMCOM`, `CORAK` and 277 other
unit types; layer 2 is live; the footprint warning fires. **`ARMCOM_DEAD` does not
exist** — see below. `tacli maps` reads 99 names off `SELMAP` and leaves the shell back
on `MAINMENU`.

**What the live tables corrected.** Every one of these was a claim before this phase and
is a measurement now:

- **The commander leaves no corpse.** Stock TA's 570 features contain no name with `com`
  in it at all: every other unit has `<name>_dead` (the wreck) and `<name>_heap` (the
  rubble), but `armcom_dead` is not among them — the Commander explodes and leaves
  nothing. The example above named it, and layer 2 caught it the first time it ran, with
  *"did you mean 'ARMCROC_DEAD'?"*. Fixed here to `armlab_dead` (5x6, 564 metal).
- **Unit table indices move between the menu and a game.** `ARMCOM` is index 162 at the
  shell and 34 in a skirmish, with the same 279 entries both times — the table is
  rebuilt, not extended. This is the case for name-keyed identity, measured: a scenario
  compiled against menu indices would spawn the wrong units.
- **Only the names load at startup.** At the menu `UnitName`, `Name` and `Side` read
  correctly while `UnitDescription` and `FootX/FootY` are empty — TA parses enough of
  each FBI to fill the menus and the rest when a game loads. `tacli units` says so out
  loud when it caches a menu-time read, because a catalogue with no footprints silently
  cannot run the spacing check.
- **The unit table's base and stride are verified, not claimed.** The walk proves them
  itself: a live unit's `UnitDefStruct*` (`unit+0x92`) must equal `defs + UnitID*0x249`,
  and `stride_check` reports `ok` / `MISMATCH` / *no units to check against*. It read
  `ok` in a live skirmish, which pins `UNITINFOCount` at `main+0x1438F` and `UnitDef` at
  `main+0x1439B` [VERIFIED]. On `MISMATCH` the CLI refuses to cache — a plausible-looking
  table in the wrong place is worse than none.
- **The map list is a shell-only read.** `MAPNAMES` is a listbox on `SELMAP`, so `maps`
  works before `Start` and says exactly that in a game. The screen push and the list's
  fill are separate frames, so the walk waits for the list instead of snapshotting once.

**C — the applier (fork). BUILT 2026-09-01.** `tagpu_scenario.c`: wire scanner,
resolve-before-create, the `0x4969D2` detour, create pass, orders, switches, camera,
result JSON. `tacli scenario apply` and `tacli switches`; `roster` gained
`UnitInGameIndex` as `idx=`.
*DoD, met in order, on `dojo1` / Two Continents*: one ARMCOM at exactly `1600,1600`,
`engine_index 2`, dead centre of the window → one `armlab_dead` on screen with TA's own
status line reading `Wreckage M:564`, the catalogue's metal value → 402 entities and 400
orders in one visit, `failed 0`, and the two waves meeting in the middle under
`Peewee: Under Attack` → **every** `actual [x,y]` equal to `requested`, the third
component the terrain snap → an unknown unit name caught twice, by the catalogue
(`units[1].type: no unit named 'ARMNOPE' — did you mean 'ARMSNIPE'?`) and, from a
hand-written wire file, by the fork (`applied 0`, naming the ordinal and the reason).

Everything else in phase C's scope was exercised the same way rather than assumed:
`on_error: "skip"` (2 of 3 created, the third named), an order targeting a **unit** handle
(the ARMROCK left its spawn and closed on the CORAK), an order targeting a **feature**
handle, `stance`, `health` (a short red bar), `nanoframe` (a translucent scaffold and a
split build bar), and `camera.pin` (which writes the eye the fork chose into
`tagpu_eye.txt`). The one thing that does not work is player resources — see below.

**What the live runs corrected.** Every one of these was a design claim before this phase:

- **Player slots are 0-based, and they are the engine's own.** TA's registry keys are
  `Player0Controller`..`Player9Controller`, `--player 0:1` lands in `Players[0]`, and the
  roster reports `own=0` for it. Phase A's schema bounded `slot` and `owner` to `1..10`
  and so could not name the human seat a plain launch uses. Now `0..9`, and the shipped
  example moved with it.
- **`clear_existing` must run BEFORE the create pass, over a snapshot.** Creating first
  looked safer — no window with an empty world — and cost 302 of a 401-unit scenario:
  `MaxUnitNumberPerPlayer` counts units that are about to die, and the engine refuses
  silently, one unit at a time. Clearing first is safe precisely because the apply point
  is *outside* the sim loop, so the simulation never observes the empty world. The
  snapshot stays: it means the sweep kills exactly what was there and can never reach a
  unit the same visit created.
- **A scenario that leaves a player with no units ends the game**, and then
  `Game_MainLoopTick` stops and the applier is never called again. That is the engine
  being right, not a bug — but the first version reported it as a silent 30-second CLI
  timeout. There is now a 600-frame watchdog in the present path that gives up and says
  *"the game's main loop never reached the apply point — apply needs a running game, not
  the menus, the mission-end screen or a paused one"*.
- **Order positions are whole world units in the screen convention**, stored verbatim —
  measured, not chosen between two disagreeing sources. See *The coordinate asymmetry*.
- **The camera must subtract half the target's altitude.** `sy = wy - altitude/2 - eyeY
  + 32`, so a target on 90-unit ground sat 45 px above the middle of the window until the
  inverse carried the height term. With it, `roster` reports the target at exactly
  `512,384` in a 1024x768 window.
- **A trigger file that outlives its game is a live hazard.** A leftover
  `tagpu_scenario.trigger` fired itself into the *next* launch's skirmish. `tacli launch`
  now deletes every stale trigger and result alongside the log rotation: triggers are
  requests, not state.
- **No spawn hitch at 400 units** (open question 4). 402 creations and 400 orders in one
  visit, 0.2 s of CLI round trip including the poll, no visible stall and no dropped
  frame. `stagger_ticks` is not needed.
- **The result file is ASCII.** The fork `\u`-escapes every byte over 0x7F one byte at a
  time, so a UTF-8 em-dash in a C string literal reaches the agent as mojibake. Comments
  may be typographic; runtime strings may not.
- **`on_error: "skip"` never reached the fork.** The fork's skip path was right from the
  start — a hand-written wire with one bad name creates the other two and reports the
  ordinal that failed — but layer 2 refused the whole file first, so no author could ever
  get there. Layer 2 now warns and passes the name through untouched. Dropping the entity
  in Python instead would mean renumbering ordinals and orphaning any order or camera that
  named it; the fork already re-checks every name, so per-entity best effort belongs
  exactly where it can be per-entity.
- **`setup.players[].metal/energy` cannot be set from the apply point** — but they *can*
  be set at launch, which phase D then found. The offsets are
  right (confirmed against a live read: `PlayerStruct` stride `0x14B` at `main+0x1B63`,
  `fCurrentEnergy +0x8C`, `fCurrentMetal +0x98`, storage at `+0xA4`/`+0xA8`) and the write
  lands — the result's `players.wrote` shows the figure back. But TA recomputes storage
  from the units a player owns and clamps the level to it every simulation tick: 4321
  against 50 storage read 50 again within a second, and so did 12. The applier still
  writes, and reports `requested` / `wrote` / `now`; the compiler warns whenever a file
  asks, so `validate` says it before anything runs; and the shipped example dropped the
  two keys rather than advertise a knob the engine overrules. Setting resources for real
  is a launch-time problem — and phase D solved it; see below.

**D — `scenario load` end to end. BUILT 2026-09-01.** `cmd_scenario_load` composes the
verbs that already worked: launch carrying `setup`'s launch half, the shell path by gadget
name, a wait for a world, a read-back of the map the engine really loaded, then `apply`.
**No fork change was needed** — phase D is composition plus the launch half `apply` drops,
and the C side is untouched.
*DoD, met on `dojo1` / Two Continents*: `tacli scenario load dojo1 200v200` goes from a
stopped instance to 402 entities, 400 orders, `failed 0` and the camera on the wreck
between the two waves in **6.4 seconds**; `glshot` shows the collision with TA's own status
line reading `Wreckage M:564` and `Rocko: Under Attack`. The `shootall` A/B is **VERIFIED**
— see *Engine switches* above; `scenarios/shootall-ab.json` ships so it can be re-run.

**What phase D added, and what the live runs corrected:**

- **`setup.unit_limit` is real, and it is a launch-time key.** `write_totala_ini` now
  composes `[Preferences]` from what is asked for instead of branching on sound alone, so
  `NoDirectSound` and `UnitLimit` coexist and neither drops the other. With
  `unit_limit: 500` the engine reads `MaxUnitNumberPerPlayer` **500** (250 unset) and sizes
  its array to `array_slots` 5000 (2500 unset) — which is exactly why raising the limit in
  a running game cannot work, and why this belongs to `load` and not to the applier.
  `launch --unit-limit` exposes the same knob on its own.
- **Unit indices are handed out in per-player blocks of the limit.** Player 0's units are
  `idx=1..500` and player 1's start at `501`, so `roster` reads back whether the limit took
  without a peek. (TAF's protocol notes say the same: a commander is `unitId % maxUnits == 1`.)
- **Open question 5 is answered: the loaded map is readable, and the fallback is real.**
  `*(main+0x391E9) + 0x204` reads `"Maps\Two Continents.TNT"` in a live game — the offsets
  from `tamem.h` are right. Asked for a map that does not exist, TA silently loaded
  **`Maps\Canal Crossing.TNT`** instead and said nothing, and with `--any-map` all 402
  entities applied to it *without one out-of-bounds error*: Canal Crossing is large enough
  that the Two Continents coordinates are all in bounds, just on the wrong terrain. That is
  precisely the silent wrongness the check exists for, so `load` compares the stem,
  case-folded, against `setup.map` and **refuses to apply on a mismatch**; `--any-map`
  overrides. Layer 2 already catches an unknown map *name* before anything launches, so
  this check covers what layer 2 cannot see: a map the shell lists and the engine then
  declines to load, and any name that reaches the registry past validation (`--map`).
- **`--player`'s fields are positional, and an empty one now means "leave that key
  alone"** — `--player 0:1::3` sets a colour without inventing a side. `setup.players`
  compiles straight to these strings rather than writing the registry a second way, so a
  scenario and a hand-typed launch cannot disagree about what slot 0 means.
- **`load` is a clean start, and refuses rather than assuming.** On an instance that is
  already running it stops nothing unless `--restart` is given, and the refusal names
  `apply` as the other thing the author might have meant.
- **A menu click is confirmed before the screen changes.** TA answers a click through its
  own `UIChange_f` on the frame the click arrives and pushes the new screen a frame or two
  later, so a settle window sized for the transition is wasted: `load` reports the screen
  it clicked *on* plus the engine's confirmation, and the next step's screen says where
  that click went. Widening the settle to 5 s changed nothing and was reverted.
- **The overlay's own `units: alive=N` is the live signal.** A non-zero count is proof of a
  world in memory. Zero is not: that line also appears at the *menu*, where the struct is
  readable and the world is not there yet, so waiting on the bare word would apply into
  nothing.
- **Starting resources are a launch-time setting, and the knob was hiding in plain
  sight.** The skirmish registry key carries `Player<N>Metal` and `Player<N>Energy`
  alongside the `Controller`/`Side`/`Color` this tool already wrote — six values a seat,
  defaulting to 1000 — and they set the player's **storage** as well as their level: 3000
  metal asked for reads `fCurrentMetal` 3000 *and* `fMaxMetalStorage` 3000, TA's own bar
  reads `3000/3000`, and it is still 3000 half a minute later. So phase C's finding was
  right and incomplete: the engine recomputes storage from owned units *in a running
  game*, which is why the applier's write evaporates, but at game start the skirmish
  setting establishes both and nothing takes it away. `setup.players[].metal/energy` now
  works through `load`, `--player` grew the two fields
  (`N:controller[:side[:color[:metal[:energy]]]]`), and the compiler's warning fires for
  every verb *except* the one that can honour it. Details and the whole key:
  `cmdline-options.md`.
- **Skirmish registry state is sticky, by design.** What a run does not name it inherits
  from the run before — that is TA's own model (the setup screen remembers), and it is
  already true of `side` and `color`. A scenario that wants a specific figure has to say
  so; `load --json` reports the exact `--player` flags it launched with, in
  `loaded.players`, so the answer is never a guess.
- **"From nothing" includes the instance.** `scenario load dojoD one-unit` on a name that
  had never existed cloned the prefix, mirrored the gamedir, launched, drove the shell and
  put the commander at exactly `1600,1600`, dead centre of the window, in **7.8 seconds**.
  Its player 1 came back as `idx=251` — the stock 250-block, next to the 501 the
  `unit_limit: 500` run produced, which is the control for that reading.

**E — `scenario dump`.** Live game → JSON: units, features, camera, setup. Orders are **not**
dumped in v1 (`UnitOrders` at `+0x5C` is an un-RE'd linked list); a dumped file is marked
`"source": "dump"` with an explicit note of what was not captured, so nobody mistakes a
situation snapshot for a savegame.

## Open questions

1. ~~**`shootall` semantics**~~ — **ANSWERED, phase D.** Six idle Peewees and an enemy
   Solar Collector in range: untouched for 45 s with the bit off, a wreck within 45 s with
   it on, and nothing moved either way. Idle units engage enemy *buildings* in range only
   with `0x400` set. `cheats` was not needed. The A/B ships as `scenarios/shootall-ab.json`.
2. **The FBI `Corpse=` offset** inside `UnitDefStruct+0xA8..0x13D` — unlocks
   `{"type": "ARMCOM", "as": "wreck"}` without a corpse-name lookup.
3. **`UnitOrders` layout** (`+0x5C`) — gates order round-tripping in `scenario dump`.
   Phase C pinned one field of it: `Pos` is a `Position_Dword` at `+0x22`, whole world
   units, screen convention, written verbatim from the order call.
4. ~~**Spawn hitch at 400 units**~~ — **ANSWERED, phase C.** 402 creations and 400 orders
   in one visit to the detour: no visible stall, no dropped frame, 0.2 s of CLI round trip
   including the poll. `stagger_ticks` is not needed and is not being added.
5. ~~**Registry map name vs reality**~~ — **ANSWERED, phase D.** `GameingState.TNTFile`
   (`*(main+0x391E9) + 0x204`) reads `"Maps\Two Continents.TNT"` live, and `load` refuses to
   apply when its stem is not the map the file asked for (`--any-map` overrides).
6. **`load`/`unload` order constants disagree between the two corpora.** TA's Delphi table
   has `unload`(5) `load`(6); TADR's C++ `ordertype` enum has `LOAD = 5` `UNLOAD = 6`. The
   compiler follows the Delphi table. Neither is exercised by anything built so far, and
   one transport unit settles it.
7. **`ActualUnitLimit` (`main+0x37EEA`) reads 0** in a stock skirmish while
   `MaxUnitNumberPerPlayer` (`+0x37EEC`) reads the expected 250 — and still reads 0 with
   `UnitLimit=500` in `totala.ini`, where `MaxUnitNumberPerPlayer` reads 500 and the unit
   array grows to match. So the field is not the live limit under any setting reached so
   far; phase D's launch path made the question sharper rather than answering it.
