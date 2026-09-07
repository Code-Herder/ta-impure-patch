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

Not present anywhere on this machine: a BOS→COB compiler, a COB→BOS decompiler, a COB VM
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
| Files | Open a stock unit by name, a loose mod folder, or a `.ufo`, all through the `ta3do` asset layer; a hires GLB may replace the 3DO (posed by node name, as the game does). A **project is a folder**: BOS, built COB, FBI, model reference. Export = `tools/hpipack.py` → `.ufo`, verified by loading it with `tacli`. The game folder lives in per-user config outside any repo. |
| Repo placement | `tools/tacob` (CLI: `compile`, `decompile`, `dump`, `run`, `edit`), `tools/tacob-edit.html`, `tools/test_tacob.py` (stdlib `unittest`, hand-built fixtures, the `test_ta3do.py` idiom), shipped headers under `tools/tacob-include/`. `/projects/` is gitignored so decompiled stock BOS is never tracked. |
| JavaScript libraries | **CDN through an import map in development** (the `ta3do-view.html` idiom); the exe build fetches pinned, checksummed copies into a gitignored folder and rewrites the import map. Nothing vendored into git. |
| The exe | **pywebview + PyInstaller onedir**, built with a Windows Python installed into the Wine prefix (no remote, no CI). WebView2 detected, default browser as the fallback. The heavy venv (torch, onnxruntime) stays out. First-run game-folder picker. |
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
  serve: local HTTP (stdlib) -> /pose, /trace, /state, /build, /events
tools/tacob-edit.html            CodeMirror 6 + three.js, loads the project's GLB (ta3do export)
tools/tacob-include/*.h          our own standard headers, values verified against the engine
tools/test_tacob.py              unittest: hand-built COBs, round trips, VM stepping
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
  `attach-unit`, `drop-unit`, `play-sound`.
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

**Rules to be measured, not assumed** `[PLANNED]`: units advanced per tick by `move` at a given
speed; whether `turn` takes the short way; `spin` acceleration; when `wait-for-turn`/`wait-for-move`
release; `sleep` milliseconds onto 30 Hz ticks; the order threads run within a tick; what a
thread does at its `RETURN` when it was `start-script`ed. Spring's `CobThread` supplies the first
draft; every difference the traces show becomes a line in this section with the numbers.

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

`tick` is the sim tick `*(main+0x38A47)`, which `tagpu_posedump.on`'s header line now stamps
too (`posedump: tick=N idx=U …`), so the two logs join on `(tick, unit)`; `unit` is the in-game
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

**Lints** (each is a snag that cost a live run): `wait-for-turn` inside `AimWeaponN`; an aim
script without the `signal` / `set-signal-mask` pair; a static peak-thread estimate (SmokeUnit +
walk + one per aim and fire script) above eight; a `Query` piece that `Create` never hides.

**Template**: "add weapon N" writes the four scripts in the slew-and-return shape — the only one
of the four measured bodies that fired evenly (19/18/25/19 shots on slots 4..7, `extra-weapons.md`).

## The editor page

Left: the BOS editor — CodeMirror, live diagnostics from the parser, completion from the 3DO's
piece names and the file's script names. Right, top: the viewport — posed model on a ground
plane, orbit/zoom as in `ta3do-view.html`, a draggable target marker, effect markers, and the
eight-thread monitor as a strip (owner script, state, and the tick a request was refused).
Right, bottom: tabs — unit state, weapon slots, console, trace. Across the top: the director bar
(scenario picker per class, raw event buttons with argument fields) and the transport
(play/pause/step, tick counter, scrub).

## Landings

| # | Landing | Gate |
|---|---|---|
| 1 | Compiler + decompiler, CLI only (`compile`, `decompile`, `dump`), preprocessor, shipped headers | **built 2026-09-07** — 278 of 278 stock COBs round-trip byte-identical as whole files; 26 offline tests green |
| 2 | The `tagpu_cobtrace.on` hook | **built 2026-09-07** (§Landing 2) — the nine scenarios each produce a cobtrace log and a posedump, kept under `research/notes/evidence/cobtrace/`; reviewed as an engine change |
| 3 | VM + director, headless `tacob run` | trace and pose diffs empty against landing 2's logs; the pool refuses the ninth thread |
| 4 | The editor page, lints, slot template | driven by hand: open a stock unit, edit, restart, see the change, pack a UFO that `tacli` loads |
| 5 | Packaging: pywebview launcher, browser fallback, game-folder picker, PyInstaller onedir | the built folder runs on a machine with no Python |
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

## Gaps this design does not close

- **Scriptor compatibility of hand-written literals** is unproven until the binary is found:
  truncation is established from its output, but not whether it computes in single precision
  (`<12.5>` sits on a boundary either way). Our compiler is self-consistent, not proven identical.
- **The effect constants** (`SFXTYPE_*`, the explosion flags) are the community's values until
  the `EMIT_SFX`/`EXPLODE` handlers' tables are read — the handlers themselves are located
  (`0x480EB0`, `0x481140`, landing 2).
- ~~**`rand`** cannot be replayed from posedump~~ — cobtrace logs every draw as a `D` line (landing 2).
- **Flight, sailing and diving** are sketches. The events they generate are exact; the path is not.
- **The interpolation rules** (§The VM) are unknown until the first traces are diffed.
