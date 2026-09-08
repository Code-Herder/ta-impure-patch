# tacob — a BOS/COB editor with the script running on its model

*Outcome of the `/grill-me` interview, 2026-09-07 (worktree `COB_EDITOR`). Goal: write a unit
script in BOS with highlighting and live diagnostics, compile it to COB, and watch it run on the
unit's own 3DO the way the engine would run it — walk, aim, fire, die — with the extended weapon
slots of [More weapons per unit](extra-weapons.md) treated as first-class. Ships as a Windows
folder that needs no Python. Companion references: `file-formats.md` §2 (the COB format this
compiles to) and §1/§3/§5 (the assets it reads), `extra-weapons.md` (the module whose protocol
the director reproduces), `model-export.md` (the `ta3do` pipeline the viewer reuses),
`model-import.md` (why a GLB may stand in for the 3DO), `tascene-design.md` (the browser-lab
idioms), `roadmap.md` (the row in "the road beyond").*

Facts tagged `[VERIFIED]` were read from the source or measured this session and say where.
`[CLAIMED]` = asserted from the community's knowledge of Scriptor and Spring, not yet checked
against this engine. `[PLANNED]` = a design decision, nothing built.

---

## The one-paragraph summary

`tools/tacob` is a stdlib-only Python tool that holds **everything that understands COB**: a BOS
parser with a C-preprocessor subset, a compiler to Cavedog's bytecode, a structured decompiler
back to BOS, a virtual machine that runs the bytecode on a model of the unit, and a **director**
that plays the engine's part — calling `Create`, `StartMoving`, `AimWeapon4(heading, pitch)` and
the rest at the engine's cadence. `tools/tacob-edit.html` is the editor and viewer: CodeMirror
for the text, three.js for the posed model on the glTF the `ta3do` exporter already writes, one
node per piece. The page edits and draws; it never interprets. Each tick it asks the local server
for one pose frame. The CLI runs headless too (`tacob run`), which is what the correctness gates
diff against the real game.

## What exists today — the premise corrected

*Landing 1 has since built the compiler, the decompiler and the gate (§Landing 1, below); this section records the starting point.*

The interview opened on "we already have a compiler/decompiler". We do not. `[VERIFIED — the
repo and `vendor/` searched 2026-09-07]`

| Have | What it is | Where |
|---|---|---|
| COB header + table reader/writer | parses the 11-dword header, rebuilds the file with recomputed offsets | `tools/cobalias.py` |
| Opcode table with operand counts, flat disassembler, script cloner with jump relocation | `cobclone.py --dump AimPrimary` lists one script; the clone relocates `JUMP`/`JUMP_NOT_EQUAL` targets and rewrites the signal bit | `tools/cobclone.py` |
| A Create-prologue walk for `HIDE`/`SHOW` | decides which pieces a render skips; deliberately decodes no jumps | `tools/ta3do` `hidden_at_create`, `shown_after_create` |
| The format | header, opcode values, GET/SET IDs 1–20, piece-name binding, thread pool | `file-formats.md` §2 |
| The assets | HPI/UFO/CCX archives, 3DO tree, GAF textures, palette, FBI fields, glTF export with **one node per 3DO piece, named after it, children kept** | `tools/ta3do` (`model_to_gltf` docstring) |

Not present anywhere on the reference setup: a BOS→COB compiler, a COB→BOS decompiler, a COB VM
(`tascene` is bind-pose *by design* — "offline there is no COB VM"), Cavedog's Scriptor, or any
`.bos` source `[VERIFIED — `find` over the checkout, `vendor/`, the game dir and the Wine
prefix]`. The owner cannot obtain the Cavedog BOS sources.

## Decisions locked

| Branch | Decision |
|---|---|
| Where the VM lives | **Python, with the compiler.** One language, one definition of the opcode semantics; the compiler's output is executed by the same code in the unit tests; the VM is verifiable against the engine's oracles from a plain script. A JavaScript VM in the page (60 fps, no per-tick call) was considered and rejected: two implementations of the semantics that would drift. The page polls one pose frame per tick over localhost, about a millisecond a call. |
| Source of truth | **BOS.** COB is a build artifact. Opening a stock unit decompiles it once into a project; from then on you edit BOS and every save compiles. |
| Round-trip gate | **Every stock COB decompiles and recompiles byte-identical** — the code words, the entry table, the script names, the piece names and the static count. Whole-file identity (the name area's layout) is only claimed against Scriptor itself. |
| Dialect | **The community's Scriptor BOS, whole**, plus a C-preprocessor subset. See §The BOS dialect. |
| Numeric oracle | **Scriptor's binary, if found, is the oracle** — run under Wine on a probe corpus, its COB diffed against ours byte for byte. Never a source of code. An open-source clone is a weaker oracle and gets labelled as one. Until then Scriptor compatibility of hand-written literals is a stated gap. |
| VM fidelity | **The engine is the reference; Spring's `CobThread` is the first draft only.** Gates are traces from the running game: `tagpu_posedump.on` (pose per piece per frame, already exists) and a new `tagpu_cobtrace.on` hook (script calls with arguments, returns and tick). |
| The director | **Events, arguments and timing exact; body motion a sketch.** Walk, aim, fire, hit, die, activate, build, and per-class flight: a straight run for ground units, a circuit for fighters, a hover for gunships, a run-in for bombers, a water-level glide for ships, a dive for submarines. The sketch feeds the aim scripts a realistic heading/pitch stream and is never presented as the engine's movement model. |
| The script's world | **A unit state panel is everything `get` and `set` touch**: unit position and heading, target position, and the twenty stock value IDs. IDs ≥ 21 (TADR/ProTA extensions, which our engine lacks) warn and return 0. `rand` is seeded and settable. Effects are markers, not physics. |
| Extended weapons | **Native**: slots are first-class objects read from the FBI, bound to their four scripts by name, aimed by the module's own protocol for slots 4+, with the eight-thread pool simulated including its silent failure. See §Extended weapons. |
| Edit vs play | **Separate states.** Typing re-parses for diagnostics only. Save or "Build and restart" recompiles and restarts from `Create`: running threads point into old code and a hot swap would show behaviour no game shows. |
| Timeline | **Recorded.** Every tick's poses and events are kept; pause, step and scrub backwards while the live head continues. |
| Trace format | **The trace tab uses the cobtrace hook's format exactly**, so the gate is a diff and a modder can diff the same way. |
| Files | Open a stock unit by name, a loose mod folder, or a `.ufo`, all through the `ta3do` asset layer; a hires GLB may replace the 3DO (posed by node name, as the game does). A **project is a folder**: BOS, built COB, FBI, model reference. Export = `tools/hpipack.py` → `.ufo` — but landing 4 measured that a `.ufo` **does not override a path a stock archive already has** and a loose file does, so `tacob pack --install <gamedir>` writes both (`file-formats.md` §5). The game folder lives in per-user config outside any repo. |
| Repo placement | `tools/tacob` (CLI: `compile`, `decompile`, `dump`, `roundtrip`, `run`, `fit-world`, `pose-check`, `open`, `gui`, `serve`, `lint`, `pack`, `weapon`), `tools/tacob-edit.html`, `tools/tacob-setup.html`, `tools/test_tacob.py` (stdlib `unittest`, hand-built fixtures, the `test_ta3do.py` idiom), shipped headers under `tools/tacob-include/`; the packaging in `tools/tacob_app.py`, `tools/tacob.spec` and `tools/tacob-build.py`. `/projects/`, `/tools/vendor/`, `/dist/` and `/build/` are gitignored so decompiled stock BOS, fetched JavaScript and build output are never tracked. |
| JavaScript libraries | **CDN through an import map in development** (the `ta3do-view.html` idiom); the exe build fetches pinned, checksummed copies into a gitignored folder and rewrites the import map. Nothing vendored into git. **Built (landing 5)**: `tools/tacob-build.py vendor` → `tools/vendor/` (gitignored) + `tools/tacob-vendor.json` (the manifest, tracked); the *server* rewrites the map on the way out, so one page has two homes. |
| The exe | **pywebview + PyInstaller onedir**, built with a Windows Python installed into the Wine prefix (no remote, no CI). WebView2 detected, default browser as the fallback. The heavy venv (torch, onnxruntime) stays out. First-run game-folder picker. **Built (landing 5)** — all of it, and the folder runs in a Wine prefix with no Python in it. |
| Decompiler failure | **Refuse loudly, per script.** Scriptor emits only structured `if`/`else`/`while`, so stock COBs recover; a COB from another compiler or a `cobclone` output may not. Name the offending word address, show the script as a listing, never emit `goto` or a raw block — BOS has none and it would not recompile. |
| Lost names | Customary parameters for known scripts (`AimPrimary(heading, pitch)`, `Killed(severity, corpsetype)`, `QueryPrimary(piecenum)`, `SetSpeed(speed)`, `RockUnit(anglex, anglez)`, the `WeaponN` family alike); otherwise `arg1`, `var1`, `static1`; signal masks numeric. |
| Order | Five landings, each with its gate — §Landings. |
| Out of scope | TA:Kingdoms COBs (version ≠ 4, the sound table), function-like macros, editing FBI or weapon TDF, a "test in game" button that launches `tacli` (a natural follow-up once pack-to-UFO exists). |

## Architecture

```
tools/tacob                      stdlib-only Python
  bos: lexer, preprocessor, parser  -> AST
  cob: assembler (AST -> words), disassembler (words -> ops), decompiler (ops -> AST -> BOS)
  vm : threads, pieces, statics, the eight-record pool, tick stepping
  director: per-class scenarios, the weapon-slot protocols, the unit state record
  serve: local HTTP (stdlib) -> /state /pose /trace /events /source, and the POSTs
          /build /transport /event /world /pack /template
tools/tacob-edit.html            CodeMirror 6 + three.js, loads the project's GLB (ta3do export)
tools/tacob-setup.html           the first-run picker: the game folder and the unit
tools/tacob-include/*.h          our own standard headers, values verified against the engine
tools/test_tacob.py              unittest: hand-built COBs, round trips, VM stepping, the paths

tools/tacob_app.py               the packaged entry point (`gui` is the default subcommand)
tools/tacob.spec                 PyInstaller onedir; the three scripts ship as *data*
tools/tacob-build.py             vendor · verify · wine-setup · build · check   (host side)
```

The page never reads a COB. It gets the piece tree from the GLB (node names = 3DO piece names,
the binding `model-import.md` describes) and poses nodes from the frames the server sends: per
piece, position (16.16 → float), the three angles (TAang → radians), visibility. `[PLANNED]`

## The BOS dialect

`[CLAIMED]` — this is the language as the TA modding corpus writes it; the compiler must accept
anything pasted from it.

- Declarations: `piece a, b, c;`, `static-var x, y;`, `var x;` inside a script.
- Piece statements: `move P to x-axis [lin] speed [lin];` / `... now;`, `turn P to y-axis <ang>
  speed <ang>;` / `... now;`, `spin P around z-axis speed <ang> accelerate <ang>;`, `stop-spin P
  around z-axis decelerate <ang>;`, `wait-for-turn P around y-axis;`, `wait-for-move P along
  x-axis;`, `show`/`hide`/`cache`/`dont-cache`/`shade`/`dont-shade P;`, `emit-sfx TYPE from P;`,
  `explode P type FLAGS;`.
- Control: `sleep ms;`, `signal m;`, `set-signal-mask m;`, `start-script S(args);`,
  `call-script S(args);`, `return (expr);`, `if`/`else`, `while`.
- Engine: `get VALUE`, `get VALUE(a, b, c, d)`, `set VALUE to expr;`, `rand(lo, hi)`,
  `attach-unit`, `drop-unit`, `play-sound`. **`play-sound` compiles but is fatal on retail TA**
  `[VERIFIED 2026-09-07]`: `0x10072000` is not in the dispatcher's chain and reaches the silent
  kill, so the thread that runs it ends there. No stock script uses it (all 278 decoded). The
  editor lints it; the compiler still emits it, because a mod built for an engine that does
  implement it must round-trip.
- Expressions: C arithmetic, comparison, `&&`/`||`/`!`, `&`/`|`/`^`/`~`; out-parameters by
  assignment (`piecenum = flare;`).
- Literals: `[x]` linear = x × 65536; `<x>` angular = x × 65536 / 360; plain integers as-is.
  **Rounding is the trap**: `<12>` is 2184.53 and Scriptor truncates or rounds — unknown until
  the binary is run. Our own round trip is exact whichever we pick, because the decompiler prints
  what recompiles to the same word.
- Preprocessor: `#include "file"`, object-like `#define`, `#ifdef`/`#ifndef`/`#else`/`#endif`,
  `//` and `/* */`. Search path: the project folder, then `tools/tacob-include/`. No function-like
  macros.
- Shipped headers: the explosion flags (`SHATTER`, `EXPLODE_ON_HIT`, `FALL`, `SMOKE`, `FIRE`,
  `BITMAPONLY`, `BITMAP1..5`, `BITMAPNUKE`), the SFX types (`SFXTYPE_VTOL`, `_THRUST`, `_WAKE1/2`,
  `_REVERSEWAKE1/2`, `_WHITESMOKE`, `_BLACKSMOKE`, `_SUBBUBBLES`), and the usual snippets
  (`smokeunit`, `hitweap`, `killhelp`, `StateChg`). **Every value gets verified against the
  engine's `EMIT_SFX` and `EXPLODE` handlers before it ships** — landing 2 located them
  (`EMIT_SFX` → the COB object's `vt+0x30` = `0x480EB0`, `EXPLODE` → `vt+0x34` = `0x481140`,
  `exe-reverse-engineering.md` §"The COB engine"), but their type and flag tables have not
  been read yet, so this is still research, not transcription. The decompiler emits these names, so a stock `Killed`
  comes back as `explode base type SHATTER | BITMAP1;`.

## The VM and the director

**Engine facts the VM reproduces** (all from `extra-weapons.md`, where they are
`[BINARY-VERIFIED]`; quoted here so the VM's constants have a source):

| Fact | Value |
|---|---|
| Thread pool | eight `0xA4`-byte records at `cob+0x1C`; `COBEngine_AllocThread 0x4B08C0` returns `-1` when all are busy; running count at `cob+0x53C` |
| Silent failure | `COBEngine_QueryScript 0x4B0BC0 → 0x4B0C40` opens with that allocation and returns without writing its result on a full pool — `AimFromWeaponN`/`QueryWeaponN` fall back to piece 0 |
| Pose fields | `PrimitiveStruct` `XPos/ZPos/YPos` 16.16, `XTurn/ZTurn/YTurn` `uint16` TAang, `Visible` bit (`file-formats.md` §2.6) |

**The rules, measured** `[VERIFIED 2026-09-07 — landing 3; every one read out of the binary at
the addresses in exe-reverse-engineering.md §"The COB engine", and every one exercised by the
nine byte-identical replays]`. The list the interview left open, answered:

| Question | Answer |
|---|---|
| Units advanced per tick by `move` at a given speed | `speed / 30`, `idiv` (truncating toward zero), added per tick; arrival snaps to the target exactly and zeroes the speed. `[1250]` = 81920000 becomes 2730666 a tick — a stock recoil crosses `[6]` in one tick |
| Does `turn` take the short way | **Yes, always.** The speed is negated when `(abs(target − current) > 0x8000) XOR (target < current)`; there is no flag and no long-way form |
| `spin` acceleration | Per tick, `accel / 30`, added to the axis's turn speed and clamped to the spin target; `spin` with acceleration 0 jumps to the target speed, `stop-spin` writes `-decel / 30` |
| When `wait-for-turn`/`wait-for-move` release | On the **tick after** the stepper zeroes that axis's speed field — the runner runs the eight records first and the stepper last, so an axis that arrives during tick *N* wakes its waiter at *N+1* |
| `sleep` milliseconds onto ticks | `ms × 30 / 1000`, truncated: `sleep 150` is 4 ticks, `sleep 100` is 3 |
| The order threads run within a tick | Slot 0 to 7, then the animation stepper. A thread woken by a lower slot's `RETURN` in the same pass does not run until the next tick; one woken by a *higher* slot has already had its turn |
| What a thread does at its `RETURN` when it was `start-script`ed | Nothing: the value stays on its stack, because only a start with a completion object (the engine's `Aim*`) has one to call. The record goes free and every thread blocked on it in a `call-script` goes runnable |
| Where in the frame the engine's calls land | The unit's own tick is `[weapons and AutoAim] → DoScriptsNow → [the movement pass]`; `SweetSpot` and `Killed` come from the attacker's tick, later still. This decides whether a run-later start steps in its own tick or the next one — see the call-site table in the engine map |

**Rules that surprised the design**: `play-sound` is not an opcode this engine implements (it
falls into the silent kill, so a script that uses it ends there); `%` compiles to a word the
dispatch cannot tell from `/`; a piece whose 3DO node has fewer than three vertices starts
**invisible** without any `hide` (`0x45AF1B`), which is every flare, wake and thrust anchor; and
an engine start writes all four argument words onto the record whatever its `argc`, so only a
*script* start leaves stale words under the locals it did not pass.

**What the replay supplies** `[landing 3]` — the honest boundary of the gate. `tacob run`
re-issues the fixture's `E` lines and is told, per start, three things the log does not carry:
which engine entry issued it (`QueryScript`, a run-now start or a run-later one) and where in
the frame it sits, both from the call-site table in the engine map, read off the binary; the
`rand` results, from the `D` lines; and the one `get` value a stock script branches on. That
last is `HEALTH`, and only the tank fixture needs it: `SmokeUnit` draws `rand(1,66)` only under
66 % and then sleeps `healthpercent × 50` ms, so the gap between two of its draws names the
health it read at the first — which makes **the tank's `D`-line ticks an input rather than a
prediction**, though not the first one (health starts at 100, so the VM predicts the tick of the
first draw from `Create`'s own arithmetic). The fighter's six `D` lines are `MoveRate2`'s
`rand(1,10)`, not health, and the other seven fixtures read no engine value at all. Everything
else in all nine logs — the slot each start lands in, the tick of every return, every signal
kill, every `call-script` block, the fall-through, the walk cycle's 19 ticks — is the VM's own
answer. The `Killed` mask this note planned turned out to be unnecessary: the uninitialised word
is one of the `E` line's own arguments, so it replays verbatim.

**The `tagpu_cobtrace.on` hook** `[BUILT 2026-09-07 — landing 2; the engine seam is
exe-reverse-engineering.md §"The COB engine"]`: `tagpu_cobtrace.c` hooks five sites inside the
COB engine — the one allocator every thread start takes (`0x4B08C0`), the thread runner's entry
(`0x4B0DA0`), the `RETURN` handler (`0x4B19D0`), the `signal` kill (`0x4B1A99`) and the `rand`
handler's call into the sim RNG (`0x4B15E0`) — byte-matched, all-or-nothing, reading only. The
`rand` draws turned out cheap: the handler calls the shared sim RNG `0x4B6C30` from one site,
so the `D` line exists and the gate scenarios need not avoid rand-driven pieces.

**The trace contract** `[BUILT — landing 2 writes it, landing 3 parses it; a field the engine
cannot supply is removed from *this table* in the same commit, never silently left blank]`.
`tagpu_cobtrace.on` in the instance's game dir (the other oracles' idiom; its contents are an
optional type filter, `ARMPW,CORAK`, empty or `all` for every unit) makes the fork write
tab-separated lines to `tagpu_cobtrace.log`, flushed whenever the tick changes; the editor's
trace tab prints the same lines:

| Line | Fields | When |
|---|---|---|
| `S` | `tick  unit  type  script  slot  source  args…` | a thread starts (a record was allocated). `source` = `E` (the engine called it through one of its by-name entries), `C:<n>` (a script's `start-script`, issued by thread `n`) or `L:<n>` (`call-script` from thread `n`, which then blocks until this thread returns); `slot` = the record 0..7; `args` comma-separated, the words on the new thread's stack in the order the caller pushed them, read before its first step |
| `R` | `tick  unit  slot  script  value` | a thread's `RETURN`; `value` = the word on its stack top (`?` if the stack is empty) |
| `X` | `tick  unit  script  source` | a start was **refused** — the eight records were all busy (the silent failure); `source` as above without the slot |
| `K` | `tick  unit  slot  script  by` | a thread killed by another thread's `signal`; `by` = the signalling slot. Its `RETURN` never runs, so no `R` follows |
| `D` | `tick  unit  slot  value` | a `rand( lo, hi )` draw; `value` = what the script received (`lo + result`) |
| `#` | free text | the header (version, filter, the columns); a parser skips it |

Lines are in event order. An `S` line is written at the first hook event after its
allocation (the arguments arrive on the record only then), so it can sit after lines of its own
tick but never after a line of a later one — a parser may still sort on the tick column. The
file is created afresh at every attach and flushed per line, so a killed process loses nothing.
A `# start dropped` line marks the one case a start is not written: the unit died between the
allocation and the next event (or the match ended), and its COB object is gone. `tick` is the sim tick `*(main+0x38A47)`, which
`tagpu_posedump.on`'s header line now stamps too (`posedump: tick=N idx=U …`), so the two logs join on `(tick, unit)`; `unit` is the in-game
index `*(i16*)(unit+0xA8)` — `tacli roster`'s `idx=` — and `type` the unit-def name, the
identifiers `tacli weapons` already prints. The headless replay writes lines with the same
fields, and the gate is `diff`. What the trace does **not** carry: the engine's asks for a
script the unit lacks (only an index of `-1` reaches the allocator), which engine function
issued an `E` start, and the `emit-sfx`/`explode`/`set`/`get` traffic (those are the VM's own
business). Two things the first traces taught about the *arguments*: a `Query*` script — and
`Killed`, which `Send_UnitDeath` runs through `QueryScript` with the caller's two locals —
carries the current values of the caller's out-slots, so `Killed`'s second argument is an
**uninitialised stack word** (a different number every run) that the replay must treat as
opaque; and an unwritten local reads whatever the record last held (`CREATE_LOCAL_VAR` only
bumps the stack index), so a script started with fewer arguments than it creates does not
read zeros — it reads the previous occupant's stack.

**The nine gate scenarios** — one run each in the real game with both oracles on, replayed by
`tacob run` with the same director events, the diff must be empty:

| # | Class | Unit (stock) | Exercises |
|---|---|---|---|
| 1 | kbot | ARMPW | walk cycle start/stop, `SetSpeed`, signals |
| 2 | tank | ARMSTUMP | `AimPrimary` slew + `wait-for-turn`, `FirePrimary` recoil, `RestoreAfterDelay` |
| 3 | building | ARMSOLAR or ARMWIN | `Activate`/`Deactivate`, `spin`, idle animation |
| 4 | death | any of the above | `Killed(severity, corpsetype)`, `explode` flags |
| 5 | fighter | ARMHAWK | aim from a banking frame, `Activate` at takeoff |
| 6 | gunship | ARMBRAWL | hover, continuous aim |
| 7 | bomber | ARMTHUND | run-in, `QueryLandingPad` |
| 8 | ship | CORBATS | multi-turret aim, `SetSFXOccupy` wakes |
| 9 | submarine | CORSUB | surface/dive events, `SFXTYPE_SUBBUBBLES` |

**The unit state record**: position, heading, target position, water level, and the stock IDs
`ACTIVATION`(1) … `ARMORED`(20) as `file-formats.md` §2.5 lists them. `get` reads it, `set`
writes it, the panel shows it, and the director derives the aim arguments from it.

**Effects**: `emit-sfx` = a labelled puff at the piece for a few ticks; `explode` = the piece
flung off with its flag names as a tag; `play-sound`, `attach-unit`, `drop-unit` = console lines.

## Extended weapons

The protocol comes from `tagpu/ddraw/src/tagpu_weapons.c` `[VERIFIED 2026-09-07]`:

| | Slots 1–3 (stock engine) | Slots 4..16 (the module) |
|---|---|---|
| Names | `AimPrimary/Secondary/Tertiary`, `Fire*`, `Query*`, `AimFrom*` | `AimWeaponN`, `FireWeaponN`, `QueryWeaponN`, `AimFromWeaponN` (1-based N) |
| Capacity | 3 | `WPN_CAP 16` — slots 0..15 |
| Aim | call `Aim(heading, pitch)`; fire only when it returns 1; Cavedog's scripts `wait-for-turn` before returning, so the return *is* the arrival test | `AimWeaponN` must return within its tick (turn, return 1); the module's `barrel_on_target()` compares mount→muzzle against mount→target, **yaw only**, `AIM_TOLERANCE 1024` of 65536 (5.6°), and holds fire until arrival (`hold_fire` counter) |
| Muzzle / mount | `Query*` / `AimFrom*` called per shot | cached after the first call (`pc_query` / `pc_aimfrom`) |
| Why | — | any aim script that waits holds one of the unit's eight threads for the whole slew; four extra turrets cannot each have one (`extra-weapons.md` snag 1 and 10) |

The editor's slot list comes from the FBI (`Weapon1..3` stock keys, `Weapon4..N`), each slot's
range and reload from its weapon TDF, both read through the `ta3do` asset layer. A slot with a
missing script is flagged before play.

**Lints** `[BUILT — landing 4; `tools/tacob lint`, and the editor's gutter]`. Each is a snag
that cost a live run, and each message says which note holds the measurement:

| Rule | Level | What it catches |
|---|---|---|
| `aim-waits` | error | `wait-for-turn`/`-move` inside an `AimWeaponN`, N ≥ 4 — the slot is aimed by `tagpu_weapons.c`, which needs the script back inside its tick (snag 1: 0/2/0/6 shots against 19/18/25/19) |
| `aim-no-signal` | warning | an aim script that waits without the `signal` / `set-signal-mask` pair: every re-aim leaks a record and the pool of eight empties in seconds |
| `thread-peak` | warning | a static estimate of concurrently live records above eight — per script, itself if it can block plus everything it starts or calls |
| `query-piece-shown` | warning | the muzzle a `Query*` hands back that neither `Create` nor `0x45AF1B` hides: the flare cone stuck on the hull |
| `set-ignored` | warning | a `set` retail TA drops — only six of the twenty ids have a case in `0x480B20` |
| `get-extension` | warning | a value id above 20: the jump table at `0x480AC4` stops there and returns 0 |
| `piece-unknown` | warning | a declared piece the model has no node for, so every move and hide on it moves nothing |

`play-sound` and `map-command` are **compile errors**, not lints: the compiler refuses them and
names `0x4B1B60`, because their operand encodings are the community's too and a file that
round-trips a guess is worse than one that will not build.

**Template**: "add weapon N" writes the four scripts in the slew-and-return shape — the only one
of the four measured bodies that fired evenly (19/18/25/19 shots on slots 4..7, `extra-weapons.md`).

## The editor page — built 2026-09-07

`tools/tacob-edit.html`, served by `tools/tacob serve <unit>`. Left: the BOS editor —
CodeMirror 6 through the import map, a BOS `StreamLanguage`, a lint gutter fed by the
server's findings, and completion over the unit's piece names, its script names and the
twenty value ids; under it the findings list, each row clicking through to its line. Right,
top: the viewport — the `ta3do` glTF on a grid, orbit/zoom as in `ta3do-view.html`, the target
marker, effect markers, and the camera following the unit; under it the **eight-record strip**,
one cell per `0xA4` record with its script and state (`run` / `wait-turn` / `wait-move` /
`sleep` / `call` / free) and the pc, sp, mask and blocked-on slot in its tooltip. Right,
bottom: tabs — **state** (every field a value-id handler reads, editable, and all twenty ids
with their live value and whether `set` writes them), **slots** (the FBI's weapons with range,
reload, which protocol drives them, shots and held shots, and the "add weapon N" template),
**events** (a button per by-name engine start, each issued through the call-site table),
**console** and **trace** (the cobtrace lines verbatim, so a modder can `diff` them against the
game's own log). Across the top: the class picker, the transport (restart, play/pause, step,
speed, tick, a scrub over the recorded timeline) and **build & restart** and **pack .ufo**.

**The page never interprets COB.** Each tick it asks for one pose frame and sets, per piece,
a translation, three Euler angles and a visibility flag on the glTF node **of the same name** —
0x45A950 binds by name and the COB's piece order is not the 3DO's tree order (ARMPW's COB piece
0 is `torso`; the 3DO's root is `ground`). The one conversion the server does for it: `ta3do`
writes glTF with the file's z negated and the engine's loaded model has the file's x *and* z
negated, so glTF is the engine's frame with x negated — which turns `Ry·Rx·Rz` into three.js's
`YXZ` Euler `(ax, ay, −az)` and a local offset into `(−x, y, z)`. At rest that reproduces the
exporter's own node translation exactly, which is the check that it is right.

If CodeMirror will not load, the page falls back to a plain textarea, says so in the findings
list, and everything else keeps working.

## Landings

| # | Landing | Gate |
|---|---|---|
| 1 | Compiler + decompiler, CLI only (`compile`, `decompile`, `dump`), preprocessor, shipped headers | **built 2026-09-07** — 278 of 278 stock COBs round-trip byte-identical as whole files; 26 offline tests green |
| 2 | The `tagpu_cobtrace.on` hook | **built 2026-09-07** (§Landing 2) — the nine scenarios each produce a cobtrace log and a posedump, kept under `research/notes/evidence/cobtrace/`; reviewed as an engine change |
| 3 | VM + director, headless `tacob run` | **built 2026-09-07** (§Landing 3) — all nine replays byte-identical to the game's own logs, all eight posedumps of the traced unit matching, the ninth thread refused |
| 4 | The editor page, lints, slot template | **built 2026-09-07** (§Landing 4) — driven by hand end to end: ARMPW opened, `Create` edited to hide its torso, rebuilt, restarted, the change seen in the viewport, packed and installed, and the game drew the Peewee without its torso (232 pixels of its own 52×56 box) |
| 5 | Packaging: pywebview launcher, browser fallback, game-folder picker, PyInstaller onedir | **built 2026-09-07** (§Landing 5) — `dist/tacob/`, 172 files and 29.7 MB, run in a Wine prefix with no `python*.exe` anywhere under `drive_c`: it decompiled ARMPW out of the game's archives, stepped the VM 30 ticks, and served the editor to headless Chrome **with the network cut off** — CodeMirror alive, the model on the canvas, the eight-record strip filled |
| 6 | Scriptor as oracle (whenever the binary turns up) | probe corpus compiled by both, bytes identical or every difference explained here |

## Landing 1 — built 2026-09-07

`tools/tacob` (compile · decompile · dump · roundtrip), `tools/tacob-include/` (`exptype.h`,
`sfxtype.h`, `smokeunit.h`), `tools/test_tacob.py` (26 offline tests, hand-built COBs and
BOS). Gate: `tools/tacob roundtrip --all` → **278 of 278 byte-identical**, and the identity is
the *whole file*, not only the code words — Scriptor's layout turned out fully regular
(`file-formats.md` §2.8), so the stronger claim came free.

What the corpus taught that the interview did not know, each now a rule the tool follows and
a line in `file-formats.md` §2.8:

- **The implicit return is decided by the last opcode, not the last statement.** A script
  ending `if( x ) { …; return (0); }` gets no trailing return and falls through into the next
  script — the engine really runs that way for ARMPW's `FirePrimary`. My first rule ("append
  unless the last statement is a return") broke on it, and the second ("last emitted opcode")
  broke on an empty `Create()` because it looked at the *previous* script's opcode.
- **No stock script uses `else`.** Every one of the 848 JUMPs is a loop back-edge. The `else`
  shape is compiled the community way and covered by a unit test, not by the corpus.
- **Literals truncate toward zero** — Scriptor's own output settles the direction the
  interview left open. What stays open is only float precision on hand-written decimals.
- **`speed` is a legal identifier** (`SetSpeed(speed)` in twelve stock units): the clause
  words are soft keywords.
- **`attach-unit` pushes three values**; the third is 0 in every stock use.
- **Names the decompiler can recover**: piece names in `Query*`/`AimFrom*` results, in
  `attach-unit`, and as the first argument of `PIECE_XZ`/`PIECE_Y`; flag names in `explode`;
  SFX names in `emit-sfx`; the engine's parameter names for the scripts it calls; callers'
  argument counts for the rest. Statics stay `static_var_N`, locals `varN`.

The decompiled stock scripts read like the community's BOS (ARMSTUMP's `AimPrimary` comes back
as the eight lines every modder knows), which is the readability the gate cannot measure.

## Landing 2 — built 2026-09-07

`tagpu/ddraw/src/tagpu_cobtrace.c` (the hook, §"The `tagpu_cobtrace.on` hook" above),
`scenarios/cob-*.json` (the nine class scenarios), `tools/cobtrace_fixtures.py` (runs them,
parks the camera, drops the posedump, keeps the files), and the fixtures with a per-class
table in `research/notes/evidence/cobtrace/README.md`. Verified by running it: every class
produced a trace and a posedump (the README records the fighter shape the engine's own
`ORDERS_CreateObject` fault ruled out, with and without the oracle), the tick advances
across a traced death, and the kbot trace was read line by line against the disassembly.

What the engine taught that the interview did not know, each now a rule landing 3's VM
follows (the addresses are in `exe-reverse-engineering.md` §"The COB engine"):

- **There is one allocator and the arguments come after it.** Every start — engine or
  script — goes through `0x4B08C0`, and every caller writes the arguments onto the new
  record only after it returns; the trace latches the start and writes it at the next hook
  event, always before the thread's first step.
- **Run-later starts step at the next tick; run-now starts finish inside their tick.**
  `SetMaxReloadTime` (started with `runNow = 0`) is logged at tick 117 and returns at 118;
  `StartMoving` and `Create` (`runNow = 1`) return in their own tick — and a run-now start
  runs *every* runnable record of that unit, with `dt = 0`, not only the new one.
- **`sleep` is `ms × 30 / 1000` ticks, truncated** (`sleep 150` = 4 ticks); the per-unit tick
  passes `dt = 1` and a sleeper wakes when its count reaches zero; the animation stepper
  runs after the eight records each tick.
- **`call-script` blocks the caller until the child's `RETURN`** (`L:<n>` lines: the kbot's
  `walk` is a 19-tick loop under `MotionControl`); a child refused by a full pool parks the
  caller for ever. `start-script` children inherit the parent's signal mask; a thread
  killed by `signal` never runs its `RETURN` (a `K` line, no `R`).
- **Locals are not zeroed.** `CREATE_LOCAL_VAR` only bumps the stack index; a script that
  creates more locals than it received arguments reads the record's previous words.
- **A script with no trailing `RETURN` runs into the next script's words** — measured, not
  only read off the bytecode: ARMSTUMP's `HitByWeapon` ends under `SweetSpot`'s name.
- **`Killed` is a query.** `Send_UnitDeath` runs it through `QueryScript` with two locals, so
  its second argument is uninitialised stack and the corpse type is read back from the
  record; the replay masks that argument.
- **`rand` is the sim RNG** (`0x4B6C30`, shared with 129 other call sites), one draw per
  `rand`, so the `D` line is the only way to replay it — and the draws are frequent:
  `SmokeUnit` under 66 % health draws every few hundred ms.
- **The effect opcodes reach vtable slots** — `EMIT_SFX` → `vt+0x30` (`0x480EB0`), `EXPLODE`
  → `vt+0x34` (`0x481140`) — so the shipped constants can now be verified against code, which
  is landing 3's or 4's job, not done here.
- **Stock scripts never fill the pool**: no fixture produced an `X` line. The refusal
  path is exercised only by extended-weapons content (`extra-weapons.md` snag 10).

## Landing 3 — built 2026-09-07

The VM and the director in `tools/tacob` (`run`, `fit-world`), the world timelines in
`research/notes/evidence/cobtrace/replay.json`, and thirteen more offline tests. Gate:
`tools/tacob run --all` → **nine of nine**, each replay's `--trace-out` file byte-identical to
the log the game wrote (4272 lines) and every piece of the eight usable posedumps matching on
`move=`, `turn=` and `HIDDEN`.

What it took that the design did not foresee:

- **The frame position of an engine start matters as much as its `runNow` flag.** A run-later
  start issued before the unit's `DoScriptsNow` steps in its own tick; one issued after it waits
  a tick. Getting `StartMoving` on the wrong side of that line moved one line of the kbot log.
  The per-unit tick's call order (`0x48ADC4` … `0x48ADEB` … `0x48AFAA`) settles it for the
  movement pass; `Activate` is measured rather than derived, because `UNITS_SetStateMask` is not
  in that function.
- **The oracle's deferral is part of the contract.** `tagpu_cobtrace.c` writes a start at the
  next hook event, reading the record's stack then; the VM latches and flushes at the same five
  points, or the `S` lines land in the wrong place.
- **The initial pose is the model builder's, not the script's.** Until `0x45AF1B` was read, every
  aircraft's flares and thrust anchors were "visible" in the replay and hidden in the game.
- **The gate got stronger than the row asked for.** "Trace and pose diffs empty" became whole
  files identical byte for byte, header included, so `diff` is the literal check and a modder can
  run the same one.

The refusal path (`X`) has no stock fixture — `tools/test_tacob.py` covers it with a synthetic
COB that starts nine sleepers, and with the `call-script` on a full pool that parks its caller
for ever.

## Landing 4 — built 2026-09-07

`tools/tacob serve` and `tools/tacob-edit.html` (§The editor page), the world the script sees,
a live director, the lints, the slot template, and four new commands: `open` (a stock unit into
`projects/<unit>/` — BOS, COB, FBI and the glTF with **every** piece kept), `lint`, `pack`
(`--install <gamedir>`) and `pose-check`. Twenty-four more offline tests, 63 in all.

**Gate, driven by hand**: ARMPW opened, `Create` edited to `hide torso`, built (the page's
findings list stayed clean, the viewport's Peewee lost its torso), packed and installed into a
`tacli` instance, `scenario load cob-kbot --restart`, and the game drew a Peewee with no torso
— **232 of the 2912 pixels** in its own 52×56 box, against 0 for the same shot with the
override removed. A bad edit was refused and left the running unit alone; an `AimWeapon4` with
a `wait-for-turn` produced the two lints it should.

What the landing had to read out of the binary, because the design had modelled it as a
dictionary:

- **`get` and `set` are `0x480770` and `0x480B20`**, and the twenty ids are now arithmetic
  rather than a table of zeros (`exe-reverse-engineering.md` §"`get` and `set`"). Three of
  them changed what the tool does: the XZ packing **adds** the z integer, so a negative z
  borrows from x and every unpacking handler adds the 1 back; `XZ_ATAN` subtracts the unit's
  own heading and `ATAN` does not; and **fourteen of the twenty ids have no `set` case at
  all**, which is a lint (`set-ignored`) rather than a modelling gap.
- **The piece transform is `0x43DEF0` composed through `0x4B6CC0`**, which fixes the rotation
  order (`Ry · Rx · Rz`), settles that the COB's axis operands are plain X, Y, Z, and shows
  `MOVE` to be a delta added before the rotation. Both of those were open questions in
  `model-import.md`; the disassembly closes them and `tools/tacob pose-check --all` checks the
  result against the engine's own posed vertex buffer — **exactly 0** on the kbot, the
  building and the ship, and on the four fast movers the same residual `tagpu_native.c`'s own
  `err=` reports on the same dump line.
- **The 3DO loader negates X and Z.** Every offset and vertex the engine holds is `(−x, y, −z)`
  of the file's. Until that was measured the composed pose was mirrored, and `PIECE_XZ` with
  it.
- **`rand` is Park–Miller**, `s = 16807·s mod 2^31−1` by Schrage's trick at `0x4B6C30`, so the
  editor's `rand` is the game's recurrence with a seed you can set, not a stand-in.

What the live runs taught, each now a rule the director follows:

- **Starting a stock aim every tick is the bug the lints are about.** Cavedog's `AimPrimary`
  opens with `signal`, so a fresh copy each tick kills the one still slewing and the turret
  never arrives — `K` lines all the way down, and 0 shots. The director keeps one in flight and
  re-issues only when the solution has moved further than the module's own `AIM_TOLERANCE`, or
  after ten seconds. That is `extra-weapons.md` snag 1's `full` row, reproduced by accident.
- **Most aircraft and the submarine have no `Aim*` script at all** — ARMHAWK, ARMBRAWL,
  ARMTHUND and CORSUB carry `QueryPrimary` and sometimes `FirePrimary`, nothing else. The
  engine aims them; a director that waits for an aim script to return 1 gets no shots for ever.
- **A unit carries only the `MoveRate` scripts its animation needs**, so the director drops to
  the nearest one that exists (ARMHAWK has `MoveRate2` and no others).
- **The FBI's `category` is a word list.** `ARM KBOT LEVEL1 WEAPON NOTAIR NOTSUB CTRL_W`
  contains the substring `SUB`, and the first version of the class guess called a Peewee a
  submarine.
- **A `.ufo` does not override a stock path; a loose file does.** Measured three ways
  (`file-formats.md` §5): the same COB as `ztacob-armpw.ufo` and as `aaa-tacob.ufo` — the two
  ends of the directory listing — changed **0 pixels**, and the loose
  `gamedir/scripts/armpw.cob` changed 232. `pack --install` writes both, and says why.

**What the director does not do**: it generates the events, and the path is still a sketch —
bounded loops per class (shuttle, circuit, hover, run-in) scaled to the unit's own weapon reach
so the target marker stays in range. Nothing about the motion was measured against a moving
unit, and it is not the engine's movement model.

## Landing 5 — built 2026-09-07

The Windows folder a modder unzips and runs. `tools/tacob_app.py` (the entry point),
`tools/tacob.spec` (PyInstaller onedir), `tools/tacob-build.py` (`vendor` · `verify` ·
`wine-setup` · `build` · `check`), `tools/tacob-setup.html` (the first-run picker), a `gui`
subcommand, and the path/config layer underneath all of it. Twenty-one more offline tests, 84
in all.

**Gate**: `tools/tacob-build.py check` — the built folder run inside a Wine prefix the check
first proves has **no `python*.exe` anywhere under `drive_c`**. Two runs there. `tacob.exe serve
armpw --ticks 30` read the game's archives, decompiled ARMPW, compiled it back and stepped the
VM (24 trace lines). Then `tacob.exe gui armpw --no-open`, driven by headless Chrome with
`--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1` — every host but loopback
unresolvable, so a CDN cannot answer for the folder — and the page came up with CodeMirror
alive (not the textarea fallback), a `<canvas>` with the Peewee on it, the eight-record strip
filled and `#status` reading `15 scripts, 15 pieces, 5 statics`. A `dist/tacob/` of 172 files
and 29.7 MB.

### What the packaging changed in the tool

The program is the same three stdlib-only scripts; what moved is **where they look**.

- **`HERE` is `sys._MEIPASS` when frozen** (`tacob._here()`). Everything the tool *ships* —
  `tacob-include/`, `tacob-edit.html`, `tacob-setup.html`, `vendor/`, and `ta3do` and
  `hpipack.py` themselves — hangs off it, and the spec's `.` destination is exactly that
  directory, so the two `SourceFileLoader` calls that let the three scripts find each other keep
  working unchanged.
- **What the *user* writes hangs off `user_dir()`** — `%APPDATA%\tacob` on Windows,
  `$XDG_DATA_HOME/tacob` elsewhere, `$TACOB_HOME` over both (that is what the tests set).
  `projects_dir()` is the checkout's gitignored `/projects/` in a checkout and `user_dir() /
  "projects"` in the bundle, because a project folder inside `Program Files` is not writable.
- **The game folder is decided in one place** (`resolve_gamedir`, called once in `main()`): the
  flag, then `$TA3DO_GAMEDIR` — which `ta3do` reads for itself, so the answer there is "do not
  override it" — then the saved config. `ta3do.repo_root()` walks parents for a directory
  holding both `tagpu/` and `pristine/`, which in a packaged folder finds nothing meaningful, so
  `load_assets()` refuses with a sentence rather than naming that guess.
- **`ta3do.die()` prints and raises `SystemExit`**, which `except Exception` lets straight past.
  `load_assets()` is the one door to the asset layer now: it checks the folder holds one of
  `totala1.hpi` / `totala2.hpi` / `rev31.gp3` first, and turns everything else into a
  `TacobError` — which the server answers as a 400 with the sentence in it, not a 500 with a
  type name.
- **`run`, `fit-world` and `pose-check` are developer gates** and the folder does not ship
  landing 2's fixtures. `fixtures_dir()` says that in one sentence instead of raising a
  `FileNotFoundError` three frames deep.

### The first run

`tacob gui` with nothing configured starts the server anyway and serves **the picker** at `/`,
on the same port the editor will use — so answering it is a reload, not a second URL. It asks
two things (the game folder, with any folder that looks like an install offered as a button, and
the unit), validates *before* it saves (`no scripts/armpw.cob in …` beats a session whose every
piece silently sits at the origin), writes `config.json`, opens the project and swaps the
session in. Until then every other route answers **503** with the same sentence. Measured in the
packaged folder: `/setup` answered `"needed": true` with its config at
`C:\users\…\AppData\Roaming\tacob\config.json`, and `/` served
`<title>tacob — set up</title>`.

The one thing the picker deliberately does *not* decide: **whether `pack` installs into that
folder**. `gui` defaults it on because that is what the window is for and `serve` leaves it off,
but either way the flag is the operator's (`--install` / `--no-install`) and the page only ever
supplies the value — landing 4's review's rule, that a request may not name where the tool
writes, with the Origin check still in front of it.

### The window

`open_window()` picks one of three and says which. **pywebview** when it can render;
**the default browser** otherwise; nothing but a printed URL if even that fails. The test that
decides is not "is pywebview installed" but **is WebView2 installed** — pywebview's Windows
backend silently falls back to MSHTML, which has no ES modules, so the page's import map would
produce a blank window and no error anywhere. `webview2_present()` reads
`SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}\pv` under
HKLM (both registry views) and HKCU. The exe is built **with a console** on purpose: the browser
fallback has no window of its own to close, so the console is what stops the server, and it is
where the URL and the lint findings appear.

### The JavaScript

`tools/tacob-build.py vendor` reads the page's own import map, fetches every entry, and follows
each file's **relative** imports transitively — which is how `BufferGeometryUtils.js` came along
(GLTFLoader imports it; nothing in the map names it). Sixteen files, 2.5 MB, mirrored under
`tools/vendor/` at the CDN's own paths, with `tools/tacob-vendor.json` recording each URL and
sha256. A later fetch whose bytes differ **stops the build** rather than shipping. Nothing is
vendored into git.

The rewrite is at *serve* time, not build time: `rewrite_import_map()` parses the map and swaps
each URL for `/vendor/<same path>` **only when that file (or, for `three/addons/`, that
directory) is actually there**, so one page works from a checkout with a network and from the
folder without one, and a half-vendored tree still loads. Mirroring the CDN's paths is what makes
the prefix entry work by the same rule as a file entry — and the CodeMirror files keep their
**bare** imports, resolved by the map, because jsdelivr's `+esm` bundles inline their
dependencies and two copies of `@codemirror/state` break CodeMirror.

### What the build environment taught

- **A Windows Python under Wine dies if its stdout is a plain file.** `Fatal Python error:
  init_sys_streams: can't initialize sys standard streams / OSError: [WinError 6] Invalid
  handle`, before it runs a line — which is what happens the moment a build is logged with `>`.
  Every Wine call in `tacob-build.py` therefore reads through a **pipe** and re-prints, and
  stdin is `/dev/null`. Hit twice: on `pip install`, then again on the build itself.
- **The frozen tool must not trust the console's encoding.** With output redirected, Python
  encodes with the machine's legacy code page and a single em dash in a message is a
  `UnicodeEncodeError` that kills the tool while it prints its own greeting. `tacob_app.py`
  reconfigures both streams to UTF-8 with `errors="replace"` before anything else.
- **Only the entry script needs a rebuild.** `tacob`, `ta3do`, `hpipack.py` and both HTML pages
  ship as *data*, so editing them is a copy into `dist/tacob/_internal/`, not a PyInstaller run.
- Python 3.11.9 amd64 installs into a `win64` prefix from the ordinary python.org installer
  (`/quiet InstallAllUsers=0 … TargetDir=C:\Python311`), and `pip install pyinstaller pywebview`
  brings pythonnet 3.1.0 in with it — all three are in the bundle. What Wine cannot supply is
  WebView2 or .NET, so **the window itself is the one path this gate does not exercise**.

## Gaps this design does not close

- **Scriptor compatibility of hand-written literals** is unproven until the binary is found:
  truncation is established from its output, but not whether it computes in single precision
  (`<12.5>` sits on a boundary either way). Our compiler is self-consistent, not proven identical.
- **The effect constants** (`SFXTYPE_*`, the explosion flags) are the community's values until
  the `EMIT_SFX`/`EXPLODE` handlers' tables are read — the handlers themselves are located
  (`0x480EB0`, `0x481140`, landing 2).
- ~~**`rand`** cannot be replayed from posedump~~ — cobtrace logs every draw as a `D` line (landing 2).
- **Flight, sailing and diving** are sketches, and so is walking. Landing 4's director
  *generates* the events rather than replaying them, and each is issued through the call-site
  table with the entry and frame position the engine uses — but the paths are bounded loops
  (shuttle, circuit, hover, run-in) scaled to the unit's own weapon reach, and **nothing about
  the motion has been measured against a moving unit**.
- ~~**The interpolation rules** (§The VM) are unknown until the first traces are diffed~~ —
  measured in landing 3, §The VM's table.
- **The replay's inputs are not all predictions** — §"What the replay supplies" names the three
  the director is given, and the tank's `D`-line ticks are the one place a fixture's own numbers
  come back out of it.
- ~~**The unit-state panel is one value deep.**~~ — closed in landing 4: all twenty ids are the
  retail handlers' own arithmetic (`0x480770`), including the composed piece transform
  `PIECE_XZ`/`PIECE_Y` need, and `set` writes only the six ids the engine writes. **What is
  still missing behind them**: the map is *flat* at a settable height, so `GROUND_HEIGHT`
  answers one number for the whole world; the only other unit `UNIT_XZ`/`UNIT_Y`/`UNIT_HEIGHT`
  can see is the target marker; and the seed `rand` starts a match with is unread, so the
  editor picks one and says which.
- **The replay gate and the live director are separate paths.** `tacob run --all` still proves
  the VM against the game's own logs; nothing proves the *director's* generated events against a
  game, because there is no oracle for "what the engine would have called here". The events are
  each issued correctly; that they are the ones the engine would issue, in that order, is not
  gated.
- **The effect opcodes are logged, not simulated.** `emit-sfx` and `explode` record
  `(tick, piece, argument)` and now also **where the piece was**, so the page draws a marker
  there; their handlers' constant tables are still unread, so the shipped `SFXTYPE_*` and
  explosion-flag values remain the community's and the editor prints the community's names.
- **The pywebview window has never been opened.** Wine supplies neither WebView2 nor .NET, so
  landing 5's gate exercised the *browser* fallback and the WebView2 probe's negative answer.
  `webview`, `pythonnet` and `clr_loader` are in the bundle and `open_window()`'s three branches
  are written, but "a window of our own on a real Windows machine" is claimed from the library's
  contract, not measured.
- **The packaged folder is unsigned and has no installer.** It is a folder to unzip; Windows
  will warn about it, and nothing updates it.
- **The picker's candidate list is a short fixed list**, not a search: the working directory,
  the folder the tool sits in and its parent, and the usual Cavedog/Steam/GOG paths on each of
  five drive letters. A game installed anywhere else is typed in, not offered.
- **Landing 4 ships no `.fbi` editing.** A project carries the FBI it was opened with and packs
  it if one is present, but nothing edits it, so "add weapon 4" writes the four scripts and
  leaves `Weapon4=` to the modder. Editing FBI and weapon TDF was out of scope by decision.
