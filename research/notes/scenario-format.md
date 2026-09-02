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
| Validation | **Three layers**: strict schema (unknown keys are errors), catalogue check against the live unit/feature/map lists, and an in-process **resolve-before-create** pass that creates nothing if anything fails. `on_error: "skip"` opts into best-effort. |
| Timing | **Detect** the trigger in the present path (existing idiom); **apply** from a sim-tick hook at `0x4969CB`, the point TADR spawns from — never mid-render, where the sort-grid walk lives. All entities in **one tick**, so the situation is reproducible. |
| Existing units | `setup.clear_existing` defaults **true**, removing the skirmish's starting commanders silently via `UNITS_KillUnit(u, 0)` — a scenario contains exactly what the file says. |
| Camera | `at` and `center_on` both mean **the centre of the window**, never the eye origin. Group targets compile to a coordinate; entity handles resolve to the unit's *actual* post-snap position. `pin` defaults false. |
| Orders | The engine's own order **names**, resolved through `ScriptAction_Name2Index`. TA has **no attack-move**; the idiom for a meeting engagement is `attack` a ground position. |
| Switches | `setup.switches` name-keyed to the `SoftwareDebugMode` bits. **`shootall` defaults on** (the universal player convention); always echoed in the result. |
| Assertions | **None.** The loader guarantees the setup and reports requested-vs-actual per entity; judging the outcome is the agent's job with `roster` / `shot` / `glshot`. |
| Round trip | The schema is **designed to be dumpable** (every field readable back out of the engine); `scenario dump` is the first follow-on, not a v1 gate. |
| Tests | The compiler is pure Python and gets tests in `tools/test_tacli.py` — the harness that already caught a real bug on its first day. |

---

## The file

```json
{
  "format": "ta-scenario/1",
  "description": "200 ARM vs 200 CORE meeting engagement on open ground",
  "seed": 20260901,
  "on_error": "abort",

  "setup": {
    "map": "Two Continents",
    "res": "1024x768",
    "unit_limit": 500,
    "clear_existing": true,
    "switches": {"shootall": true, "noshake": true},
    "players": [
      {"slot": 1, "controller": "human", "side": "arm",  "color": 0, "metal": 5000, "energy": 5000},
      {"slot": 2, "controller": "ai",    "side": "core", "color": 1}
    ]
  },

  "groups": [
    { "id": "arm_wave", "owner": 1, "at": [1000, 1200],
      "pattern": {"kind": "grid", "cols": 20, "spacing": 24, "jitter": 6},
      "composition": [{"type": "ARMPW", "count": 150}, {"type": "ARMROCK", "count": 50}],
      "facing": 90,
      "orders": [{"cmd": "attack", "to": [2400, 1200]}] },

    { "id": "core_wave", "owner": 2, "at": [2400, 1200],
      "pattern": {"kind": "grid", "cols": 20, "spacing": 24, "jitter": 6},
      "composition": [{"type": "CORAK", "count": 200}],
      "facing": 270,
      "orders": [{"cmd": "attack", "to": [1000, 1200]}] }
  ],

  "units": [
    { "id": "hero", "type": "ARMCOM", "owner": 1, "pos": [900, 1200],
      "facing": 90, "health": 60, "stance": "hold" }
  ],

  "features": [
    { "id": "the_wreck", "type": "ARMCOM_DEAD", "pos": [1700, 1200], "facing": 45 }
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
  half-built look.
- `stance` is a token (`hold` / `manoeuvre` / `roam`), never a raw mask.
- `orders` at group level apply to every member; a unit-level `orders` overrides. Targets
  are a coordinate (`to`) or a handle (`target`).
- Patterns in v1: `grid`, `line`, `random` (in a rect). All seeded.
- `owner` and player `slot` use the **same numbering as `tacli --player`** — no third
  convention.
- Aliases accepted: `guard` → TA's `defend`.

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

Its precise effect — idle/moving units also engage **buildings**, not only mobile units — is
the community meaning and matches the player idiom, but the engine read site has not been
found. **[CLAIMED — A/B it live and correct this table.]**

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
| name → type index | scan `taPtr->UnitDef[i].UnitName` over `UNITINFOCount` | — | What TADR's spawner does. `UNITINFO_Name2ID 0x488B10` exists but the scan is the proven path and needs no call. |
| create a unit | `UNITS_CreateUnit(owner, typeIdx, x, height, y, fullHp, stateMask, unitNumber)` → `UnitStruct*` | `0x485F50` | Pass `unitNumber = 0` and let the engine allocate; record what comes back. |
| snap to ground | read `FeatureMap[(x>>20) + (z>>20)*FeatureMapSizeX].height` | — | TADR's own height rule; feature cells are **16 world units**. |
| feature name → id | `FeatureName2ID(name)` | `0x422DD0` | |
| place a feature | `SpawnFeatureOnMap(gridPos, corpseIdx, position, volume, playerId)` | `0x423C50` | Silent and instant — this is how wrecks are placed. |
| order name → index | `ScriptAction_Name2Index(name)` | `0x438760` | Keeps orders name-keyed like everything else. |
| issue an order | `Order2Unit(scriptIdx, shiftKey, unit, targetUnit, position, p1, p2)` | `0x43AFC0` | |
| remove a unit silently | `UNITS_KillUnit(unit, 0)` | `0x4864B0` | Mode `0` = the "recreate proc" path (no explosion). Mode `3` is a normal death **with** wreckage. |
| apply point | sim-tick hook | `0x4969CB` | TADR's `GameTickHook` site; its deferred spawner runs from here. |
| map extents | `MapWidth/Height`, `MapSizeX/Y`, `FeatureMapSizeX/Y` | `main+0x14233…` | Bounds-checking source. |

**Unit fields written after creation** (`vendor/TADR/src/DDraw/tamem.h:986-1103`):

| Field | Offset | Note |
|---|---|---|
| heading | `+0x66` | `Volume_Word{bank, heading, pitch}` at `+0x64`. Full circle `0x10000`, quarter `0x4000`, **default `0x8000`**, increases CCW from above. |
| position | `+0x6A` X, `+0x6E` **altitude**, `+0x72` map depth | Three 16.16 dwords. **Naming trap**: tamem uses the screen convention (`XPos/ZPos/YPos`), Ghidra transposes it. Same bytes; do not "fix" either. |
| type index | `+0xA6` `UnitID` | The unit **type**, not the instance. |
| instance slot | `+0xA8` `UnitInGameIndex` | **Recycled on death — never a public identity.** |
| health % | `+0xF6` `HealthPerA` | |
| owner slot | `+0xFF` `cOwnerID` | |
| build fraction | `+0x104` `Nanoframe` | |
| health | `+0x108` | |
| state mask | `+0x110` | `0xc0000` hold/manoeuvre/roam, `0x20` nanoframe, `0x10000000` alive |

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

```
v1 map=Two_Continents
unit  0 ARMPW    1 1200  900 -  180 100 0     # ord name owner x y height facing hp% flags
feat  0 ARMCOM_DEAD  2400 900 -  90
order 0 attack 2400 900
sw    shootall=1 noshake=1
cam   1200 900
```

The DLL re-validates every field regardless of what the CLI promised — bounds against the
live map extents, name lookups that can fail, a hard ceiling on entity count — because the
file is on disk and can be stale or truncated.

**Result JSON**:

```json
{"applied": 401, "failed": 0, "switches": {"shootall": true, "noshake": true},
 "handles": {"hero": {"ord": 0, "engine_index": 17,
                      "requested": [900, 1200], "actual": [900, 1203]}},
 "camera": [1700, 1200]}
```

---

## CLI surface

```bash
tools/tacli scenario list
tools/tacli scenario validate 200v200                  # schema + catalogue, no game
tools/tacli scenario expand  200v200 --json            # the flat entity list, no game
tools/tacli scenario load    t1 200v200 --json         # launch → menus → live → applied → camera
tools/tacli scenario apply   t1 reinforcements --json  # mutate a live game
tools/tacli scenario dump    t1 -o /tmp/captured.json  # (phase E)
tools/tacli units    t1 --json                         # live unit catalogue (cached)
tools/tacli features t1 --json
tools/tacli maps     t1 --json                         # from SELMAP's MAPNAMES via `ui`
tools/tacli switches t1 shootall=on noshake=on
```

Scenarios live in **`scenarios/` at the repo root**, tracked in git, plain `.json`. A bare
name resolves to `scenarios/<name>.json`; anything with a `/` or a `.json` suffix is a path.
Every verb takes `--json` and exits non-zero on failure. `setup` may carry launch options;
explicit CLI flags win, so a scenario re-runs at a different resolution without editing.

**Deliberately not added**: an inline `tacli spawn t1 ARMPW 1200,900`. It would be a second
path into the engine with its own validation story, and `scenario apply /tmp/one.json`
already covers it through the safe one.

---

## Phases and DoD

**A — the compiler (Python only).** Schema, strict validation, seeded expansion, patterns,
handle resolution, wire writer. `list` / `validate` / `expand`.
*DoD*: `expand 200v200 --json` yields 401 entities byte-identical across runs; unknown unit,
out-of-map coord, duplicate handle, unknown key and `"attack-move"` each produce one clear
error; `python3 tools/test_tacli.py` passes with no wine and no game.

**B — catalogues (fork + CLI).** `tagpu_units.trigger` → `tagpu_units.json` walking
`UnitDef[]`; the same for `FeatureDef[]`; `tacli maps` from `SELMAP`'s `MAPNAMES` via
`ui --json`. Per-instance cache.
*DoD*: stock TA lists `ARMCOM`/`CORAK` and `ARMCOM_DEAD`; validation layer 2 goes live; a
footprint-vs-spacing warning fires on a deliberately too-tight grid.

**C — the applier (fork).** Wire scanner, resolve-before-create, the `0x4969CB` hook, create
pass, orders, switches, camera, result JSON. Roster gains `UnitInGameIndex`.
*DoD, in order*: one ARMCOM appears where asked → one wreck appears → 200v200 spawns in one
tick and fights → result JSON's actual positions match requested within terrain snap → a
scenario naming a nonexistent unit creates **nothing** and says why.

**D — `scenario load` end to end.** Launch, `ui click SINGLE/Skirmish/Start`, wait for live,
drop the trigger, read the result, set the camera.
*DoD*: one command from nothing to a live 200v200 with the camera on the collision point,
verified by `glshot`; and the `shootall` A/B recorded here as VERIFIED or corrected.

**E — `scenario dump`.** Live game → JSON: units, features, camera, setup. Orders are **not**
dumped in v1 (`UnitOrders` at `+0x5C` is an un-RE'd linked list); a dumped file is marked
`"source": "dump"` with an explicit note of what was not captured, so nobody mistakes a
situation snapshot for a savegame.

## Open questions

1. **`shootall` semantics** — bit `0x400` is certain; what the engine does with it is
   community lore. A/B on a live instance (blob + building in range, bit off vs on). If it
   appears inert, set `cheats` (`0x2`) alongside it before concluding anything.
2. **The FBI `Corpse=` offset** inside `UnitDefStruct+0xA8..0x13D` — unlocks
   `{"type": "ARMCOM", "as": "wreck"}` without a corpse-name lookup.
3. **`UnitOrders` layout** (`+0x5C`) — gates order round-tripping in `scenario dump`.
4. **Spawn hitch at 400 units** in one tick. Measured in phase C; a `stagger_ticks` knob
   exists as a fallback but costs reproducibility.
5. **Registry map name vs reality** — TA falls back silently on an unknown `SkirmishMap`.
   Phase D verifies the loaded map by reading `GameingState.TNTFile` after Start.
