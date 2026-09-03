# Reverse-Engineering TotalA.exe

*Research date: 2026-08-31. Scope: the shipped Cavedog binary only. Engine
re-implementations (Spring/Recoil, TA3D, Zero-K, OpenRA) are deliberately excluded.*

## Summary

**The reverse-engineering record for `TotalA.exe` is far richer than the public
search-engine surface suggests, but almost all of it lives in exactly one place**: the
source tree of **TA Demo Recorder (TADR)**, <https://github.com/tanvanman/TADR>. Not a
hobby stub — an actively developed (last push 2026-08-31), ~580-file C++/Delphi project
shipping as `tdraw.dll`, a DirectDraw shim that TA loads and which then rewrites the
running game's code and data in memory. It underpins the TA Forever multiplayer
ecosystem.

TADR contains roughly **470 distinct annotated code addresses**, a ~1,980-line header
(`tamem.h`) describing the engine's data structures with `static_assert`-checked field
offsets, and a table binding ~86 engine routines to their addresses. The community *has*
mapped the binary in depth; it simply never published that as a Ghidra or IDA database.

Two things matter up front for any modding plan:

1. **No public Ghidra project (`.gpr`), IDA database (`.idb`), or symbol map for
   `TotalA.exe` was found anywhere.** GitHub code and repo search across many query
   shapes returned nothing. `tareversing/tareversing` (2012) and
   `cjg38340/TotalA_exe-Rewrite` (2026) are both **empty repositories containing only
   a README** — abandoned intentions, not artifacts. [VERIFIED — negative result]
2. **The engine already contains an extensible, registrable command table**, and TADR
   demonstrably adds new `+commands` to the live game through it. This is the single
   most valuable extension point in the binary. [VERIFIED]

A caveat shaping the whole document: **tauniverse.com is entirely unreachable.** Every
URL there sits behind a Cloudflare JS challenge returning HTTP 403 to direct fetch, to
curl with browser headers, and to the r.jina.ai proxy; web.archive.org is also blocked
here. TAU is the main TA community archive, so part of the historical record could not
be read first-hand. Claims traceable only to search snippets of TAU threads are marked
[CLAIMED].

## Binary anatomy (PE, imports, toolchain, protection)

From a third-party static analysis of a retail `totala.exe`
([Hybrid Analysis](https://hybrid-analysis.com/sample/c94faf30b8a15a2f92f885c6f0f972cbadda765b4a920f87977e90f0e5538048/5f4a4258402fb1097172bb74)):
[VERIFIED]

| Property | Value |
| --- | --- |
| Format | PE32 executable (GUI), Intel 80386, MS Windows |
| Size | 1,178,624 bytes (1.1 MiB) |
| Toolchain | Microsoft Visual C++ 5.0 (**not** Watcom) |
| Link timestamp | 1998-07-30 18:22:29 UTC |
| Sections | `.text` (vsize 0xFA92A / raw 0xFAA00), `.rdata` (0x468C/0x4800), `.data` (0x2A65C/0x10A00), `.tls` (0x14/0x200), `.rsrc` (0xA58/0xC00) |
| Imports | ADVAPI32, **DDRAW**, **DPLAYX**, **DSOUND**, GDI32, KERNEL32, SHELL32, **smackw32.DLL**, USER32, WINMM |

Notes on this table:

- The DirectX surface is **DirectDraw (2D only — no Direct3D import), DirectSound and
  DirectPlay**. TA's renderer is a software rasteriser blitting to DirectDraw surfaces;
  TADR's whole injection strategy depends on this, as it masquerades as `ddraw.dll`.
- `smackw32.DLL` is RAD Game Tools' Smacker video codec — the FMV cutscenes.
- The report's "Software packing (MITRE T1045)" tag is a **generic heuristic and is
  wrong**. The binary is not packed or protected: radare2 auto-identifies 391+
  functions directly, and TADR patches raw instruction bytes at fixed addresses with no
  unpacking step. [VERIFIED — by contradiction from two independent RE efforts]
- **Image base 0x400000; VA↔file-offset delta 0x400C00**, confirmed twice
  independently: TADR's `ta entry point.txt` records "Point to hook; 0x49EDDD / File
  offset; 0x9E1DD" (difference exactly 0x400C00), and the section geometry above gives
  the same (`.text` VA 0x401000, raw pointer 0x400). So **file_offset = VA − 0x400C00**.
  [VERIFIED]
- Debug strings and format-string remnants survive — see the cheat-command section. No
  evidence of RTTI or a PDB (consistent with a 1997 MSVC5 C-style codebase).
- **TA:CC vs. base 3.0/3.1: unresolved.** The 1998-07-30 timestamp is post-CC, so the
  analysed sample is a v3.1-era build. Whether CC shipped a structurally different
  `TotalA.exe` or merely bumped it to 3.1 is **unverified** (Fandom returned HTTP 402;
  TAU unreachable). All RE work below targets the single build the community treats as
  canonical.

## Existing reverse-engineering projects & artifacts

**`tanvanman/TADR` — TA Demo Recorder / `tdraw.dll`** [VERIFIED]
<https://github.com/tanvanman/TADR>. Forked from `svn.riouxsvn.com/tadr`; credited
contributors Rime, Xpoy, N72, Fnordia, SJ, Yeha. Mirror at `Skirmisher/tadr`. Key
files for anyone starting out:

- `src/DDraw/tamem.h` (~1,978 lines) — the engine's data structures.
- `src/DDraw/tafunctions.h` + `src/DDraw/HardCodeFunctions.cpp` — ~86 named engine
  functions bound to their addresses.
- `src/DDraw/TABugFix.cpp` — the largest patch collection; each patch carries the
  original IDA disassembly listing in a comment.
- `src/DDraw/EngineLimits.cpp` — relocates the engine's fixed-size effect pools.
- `ta info.txt`, `ta entry point.txt` — raw working notes with `sub_XXXXXX` addresses,
  proving IDA (not Ghidra) was the tool of record.

**`ioma8/totala-re`** [VERIFIED, but treat with caution]
<https://github.com/ioma8/totala-re>. An October 2025, largely AI-assisted radare2
pass. Its solid contribution is the **HPI/SQSH archive format**, with a working Python
parser and a round-trip assembler producing byte-identical archives. Its *engine*
claims are weaker and partly speculative — e.g. a "base cadence of 100 ms (≈10 FPS)"
for the main loop, contradicting TA's known 30 Hz sim tick (TADR uses
`GameTime == 6 * 30` for "6 seconds"). **Prefer TADR where the two disagree.**

**Independent cross-verification of the entry point:** TADR gives hook point 0x49EDDD
inside `main` with core call target 0x49E830; `totala-re`, working separately in
radare2, reports `main` at 0x0049EDA0 calling `fcn.0049E830`. Two unconnected efforts,
same call graph. [VERIFIED]

**Not found:** any `.gpr`/`.idb`/`.map`/symbol export; any decompilation-to-source
project with actual content; any wiki systematically documenting offsets.

## Documented patch offsets

TADR is effectively a large published offset table, but it applies patches to memory
at runtime rather than to the file on disk. Its patch records are self-validating: each
carries `expected` bytes that are compared against live memory before writing, so the
addresses are known-good against the canonical build. Representative examples, all
verbatim from `src/DDraw/TABugFix.cpp`: [VERIFIED]

| VA | Original → patched | Purpose |
| --- | --- | --- |
| `0x004866E8` | `75 04` → `0F 84 6B 07 00 00` | Null-unit dereference on unit death |
| `0x00438EDE` | `0F 8C 69 01 00 00` → `0F 8E ...` (`jl`→`jle`) | Zero-radius circle draw bug |
| `0x0041D4CD` | → `90 90 90 90 90 90` | CD check (`CrackCdAddr`) |
| `0x0041D6B0` | → `90 90 B0 2E` | CD check, second site |
| `0x00501DF4` | → `02` | **Data**: run-level byte of the `+lostype` command entry |

The `0x00501DF4` entry is the most instructive: it is a *data* address holding a
single byte, and the code comment reads `// 1=normal; 2=cheat; 7=debug`. That is a
per-command permission byte sitting in a static table in `.data`.

**File-offset patching (the classic community technique).** The widely repeated
unit-cap edit is: open `TotalA.exe` in a hex editor and change the values at file
offsets **0x90A50 and 0x90A60**, which must be set identically. [CLAIMED — sourced
from a
[Steam discussion](https://steamcommunity.com/app/298030/discussions/0/597405278050241107/)
summary; the underlying TAU thread is unreachable and I could not read the bytes
myself.] Applying the verified delta, these correspond to VAs **0x491650 and 0x491660** —
which land plausibly close to `fcn.004916a0`, the routine `totala-re` independently
identified as the game-update/resource entry. Consistent, but **not confirmed**.

**The supported alternative is a config file, not a hex edit.** A `TA.ini` /
`TotalA.ini` in the game folder is read for: [CLAIMED — verbatim from a user-quoted
ini in a [Steam thread](https://steamcommunity.com/app/298030/discussions/0/1842367319524387998/)]

```ini
; Unit limit per player.  Set from 20 - 6553 (above 1500 may cause instability)
; TA v3.1 default is 250 ; TA v3.9.02 default is 1500
UnitLimit = 1500;
; Pathfinding cycles. TA v3.1 default is 1333 ; TA v3.9.02 default is 66650
AISearchMapEntries = 90050;
```

Caution: in the same threads a user reports `UnitLimit` having **no effect** on a
stock Steam install. The most probable reading is that ini parsing for these keys is
itself part of the unofficial **v3.9.02 patch**, not of retail v3.1 — i.e. these are
knobs added by a patched exe, not latent retail features. Treat as unresolved.

## Built-in cheat/console command surface

This is the richest extension point in the binary, and the evidence is unusually good.

**The command table is a real, registrable structure.** From TADR: [VERIFIED]

```c
typedef void (__stdcall* InternalCommandFunctionPtr)(char* argv[]);
enum InternalCommandRunLevel {
    CMD_LEVEL_NULL = 0, CMD_LEVEL_NORMAL = 1,
    CMD_LEVEL_CHEATING = 2, CMD_LEVEL_DEBUG = 4
};
struct InternalCommandTableEntryStruct {
    const char* name; InternalCommandFunctionPtr function;
    InternalCommandRunLevel runLevel;
};
```

- **`InitInternalCommand` at `0x004B7760`** takes a pointer to such a table,
  NULL-name-terminated. **A patcher can register entirely new commands by calling it.**
- **`CallInternalCommandHandler` at `0x00417B50`** — `(const char* command, int level)`.
- `dddta.h` documents a parallel entry point: `InterpretCommand(char* Command, int Access)`,
  with `// Access 1 = no cheats, 3 = cheats`.
- **`IsCheating` is a `BOOL` at `0x005091CC`** — a one-DWORD global cheat toggle.
- `TAdynmemStruct::SoftwareDebugMode` (offset `0x37F2F`) gates debug behaviour;
  TADR forces `IsCheating = TRUE` in single-player campaign so "every developer cheat
  is available".

**TADR proves extensibility in practice.** `src/DDraw/AutoTeam.cpp` defines a brand-new
`+autoteam` command and registers it via `InitInternalCommand`, and it works in the
shipped product. This is not theory. [VERIFIED]

**A genuine developer back-door exists.** A TADR code comment documents the sync
string **`+now Film Chris Include Reload Assert`**, which "flips `SoftwareDebugMode`
bit 2 unilaterally at `TotalA.exe:0x00416E90`". "Chris" is presumably a Cavedog
developer's name. [VERIFIED — as a documented finding in TADR; I have not executed it]

**The command list is longer than the famous cheats and includes debug commands.**
A published alphabetical list ([the-spoiler.com](https://the-spoiler.com/STRATEGY/Cavedog/total.annihilation.2.html))
is self-evidently a **strings dump of the binary**, not a curated cheat list: it is
alphabetically ordered, most entries lack any description, and several retain their
**printf format specifiers** — `+LINE%D`, `+TABLE%D`. Nobody types those; they are raw
table names. Beyond the well-known `+ATM`, `+RADAR`, `+NOWISEE`, `+DOUBLESHOT`,
`+HALFSHOT`, `+CLOCK`, `+VIEW #`, `+KILL`, `+IWIN`, `+ILOSE`, `+SHADOW`, `+SING`,
`+CONTOUR #`, `+CONTROL #`, `+DITHER`, `+LOS`, `+CDSTART`/`+CDSTOP`, it exposes an
undocumented developer tier: **`+ASSERT`, `+DEBUGBREAK`, `+DPRINT`, `+MEM`,
`+MEMDUMP`, `+PROFILE`, `+NETSTATS`, `+NUMLINES`, `+NUMTABLES`, `+TABLEINFO`,
`+RCACHE`, `+QUERYPRIMARY`/`+QUERYSECONDARY`/`+QUERYTERTIARY`,
`+AIMFROMPRIMARY`/`SECONDARY`/`TERTIARY`, `+SWEETSPOT`, `+ZBUFFER`, `+SELBOXES`,
`+ANTIALIAS`, `+LOSTYPE`, `+SEALEVEL`, `+FILM`, `+FILMSPEED`.** [VERIFIED as a
strings-derived list; individual behaviours are [CLAIMED] — most have no published
description.] The `CMD_LEVEL_DEBUG = 4` run level and the `+lostype` run-level byte at
`0x501DF4` corroborate that these are gated table entries rather than folklore.

No evidence was found of a separate debug *build* ever leaking, nor of a graphical
dev console — the chat bar is the console.

## Mapped internal data structures

All from TADR's `tamem.h`, guarded by `static_assert` on field offsets and `sizeof` —
so machine-checked against the real layout rather than guessed. [VERIFIED]

| Structure | Facts established |
| --- | --- |
| `TAdynmemStruct` | The "god object". Reached via the global pointer-to-pointer at **`0x00511DE8`** (`*(TAdynmemStruct**)0x511DE8`). Enormous — mapped fields run past offset `0x142CB` (~82 KB). |
| `UnitStruct` | **`sizeof == 0x118`** (280 bytes). Pos x/z/y at 0x6A/0x6E/0x72; grid pos 0x76; footprint 0x7E/0x80; sort-bucket ptr 0x82 and link 0x8E; `UnitType` (→`UnitDefStruct`) 0x92; owner 0x96; cloak state 0x10E; `UnitSelected`/state mask 0x110. |
| Unit array | `TAdynmemStruct::BeginUnitsArray_p` / `EndOfUnitsArray_p` (the latter at struct offset `0x1435B`) — a **flat contiguous array**, indexed by `short UnitInGameIndex`. |
| `PlayerStruct` | `Players[10]` inline at `TAdynmemStruct+0x1B63`, ending at `0x2851` → **~322 bytes each**. LOS buffer ptr at 0x7C, LOS tile w/h at 0x80/0x84. |
| Resource accounting | Inside `PlayerStruct`: `fCurrentEnergy`, `fEnergyProducton`, `fEnergyExpense`, `fCurrentMetal`, `fMetalProduction`, `fMetalExpense`, `fMaxEnergyStorage`, `fMaxMetalStorage` as **`float`**; lifetime totals (`fTotalEnergyProduced`, `fEnergyWasted`, …) as **`double`**. |
| `UnitDefStruct` (FBI) | **`sizeof == 0x249`** (585 bytes). CRCs at 0x13E/0x142/0x146; `buildLimit` 0x15A; **`weapon1/2/3` at 0x1EE/0x1F2/0x1F6**; `nMaxHP` 0x1FA; sight/radar/sonar 0x202/0x204/0x206. |
| Map / features | `FeatureStruct` is a **13-byte (`0x0D`) per-tile record**; `FeatureMapSizeX/Y` at `0x14233`/`0x14237`; `MAPPED_MEMORY_p` `0x14273`; `FeatureMap` `0x14287`. |
| Spatial index | `SortGridBucket`, stride `0x0A`, list head at +0x06; buckets ptr at `0x1429F`, cols at `0x142A3`, plus a dedicated off-map bucket at `0x142B7`. |
| Projectiles | Count at `TAdynmemStruct+0x141F3`; **each projectile is `0x6B` (107) bytes**. |

## The camera module — mapped by us

[MEASURED 2026-09-03, this project — disassembly of the pristine build plus live reads, not
from any vendor corpus. Established while making the camera's range follow the zoom
(`gpu-status.md` §2.3c); the community corpora name only `0x41C3C0`, and name it for its tail.]

**"Eye"** is the world position the viewport's top-left corner shows. **"View"** is `W`/`H` at
`main+0x37E37`/`+0x37E3B`. The module keeps *two* positions: the eye, and a **scroll target**
the eye is eased toward.

| VA | What it is |
| --- | --- |
| **`0x41C3C0`** | **The eye clamp.** `eyeX = clamp(eyeX, 0, mapW − W)`, the same for Y, then `0x466B70(main+0x142CB)` to refill the minimap's view rect — every path through it ends in that one call. TADR calls it `ScrollMinimap`, which describes the tail rather than the job. **12 call sites:** `0x41C59F`, `0x41C898`, `0x41C9CC`, `0x41CC16`, `0x41CC37`, `0x41CC52`, `0x41CDE1`, `0x41D054`, `0x41D184`, `0x41D26B`, `0x41D319`, `0x41D459`. |
| `0x41C450` | The same clamp shape for the *target* pair — and it has **no callers**. Dead code; an `E8`/`E9` scan of `.text` finds nothing pointing at it. |
| `0x41C4C0` | `SetCamera(x, y, smooth)` — writes the target, then clamps it **inline** against `[0, map − view]` without calling `0x41C3C0`. Callers: `0x495C68`, `0x495E11`, `0x497060`, `0x4978C9` (game-screen entry / load). |
| **`0x41CA30`** | **The per-frame camera stepper.** With no follow object it takes `je 0x41CB4A`. Where eye ≠ target it sets bit 1 of `main+0x142F1` ("camera moved"), **clears bit 3 of `main+0x14281`** — the screen fog grid's own is-current flag — then moves the eye *halfway* toward the target, capped at ±`0x140` (320 px) per axis per frame, and hands the result to `0x41C3C0`. It never writes the target. |
| `0x41CAF7` | Inside the stepper: the camera-**follow** target, recomputed every frame from the tracked unit as `pos − view/2` and clamped **inline** to `[0, map − view]`. |
| `0x41C7F7` | Smooth centre-on; clamps its target inline the same way. |
| `0x41CF10`…`0x41D060` | The scroll poll — see the table below. |
| `0x466B70` | Fills a RECT with the minimap's view box from the eye and the view size in map cells (`main+0x1423B`/`+0x1423F`). Pure computation; its only two call sites are inside `0x41C3C0`. |

**The scroll poll.** Position source is `GetCursorPos` (IAT slot `0x4FC2E0`), clamped to the
screen. Four independent directions, each firing on *hotkey* **or** *pointer on an exact screen
edge* — the hotkeys go through `KeyboardHotkeySampler` `0x4C1B80`, the same sampler `markown`
uses for SHIFT (`0xF9`):

| Direction | Hotkey id | Mouse condition | Site |
| --- | --- | --- | --- |
| left | `0xF4` | `x == 0` | `0x41CF87` |
| up | `0xF5` | `y == 0` | `0x41CFCE` |
| right | `0xF6` | `x == (main+0x37E1F) − 1`, i.e. screenW − 1 | `0x41CFA5` |
| down | `0xF7` | `y == (main+0x37E23) − 1`, i.e. screenH − 1 | `0x41CFFF` |

Two things follow, and both cost time to learn the hard way:

- **The trigger is an exact equality on the outermost pixel, not a band.** Measured on a
  1024-wide screen at 1×: a pointer at `x = 1023` scrolls right, at `x = 1020` it does not.
  (This is also why edge scroll looks dead under injected input — it is not, you just have to
  land on the last pixel. See `ta-drive`.)
- **At zoom > 1 the right edge cannot fire, and only the right edge.** `fake_GetCursorPos`
  hands the engine the *unzoomed* `u`, and three of the four screen edges lie **outside** the
  viewport rect (`L=128`, `T=32`, `B=screenH−33`), so the transform passes them through as
  identity. The screen's right column, though, *is* the viewport's right column, so it gets
  contracted toward the centre. Measured at 2× on 1024×768: a pointer at `x=1023` reaches the
  engine as **800**, so `x == 1023` is unsatisfiable, while left, up and down all still scroll.

### Fields this module owns

| Where | What |
| --- | --- |
| `main+0x1431F` / `+0x14323` | eyeX / eyeY |
| `main+0x14327` / `+0x1432B` | **the scroll target** (`MapXScrollingTo`) the stepper eases the eye toward. Every reference to it in `.text` is inside `0x41C4xx`–`0x41D4xx` — 30 and 28 respectively, and **no drawing code reads it**, which is what makes it camera-local. |
| `main+0x142F1` bit 1 | set by every camera-module path that moves the eye: the operand appears at `0x41C59A`, `0x41C893`, `0x41C9C7`, `0x41CB62`, `0x41CBD2`, `0x41CDDD`, `0x41D04F`, `0x41D17F`, `0x41D266`, `0x41D314`, `0x41D454` — one per eye writer, immediately before its `0x41C3C0` call — plus minimap/GUI readers in `0x466xxx`. Name *[INFERRED]* ("the camera moved this frame"); what is measured is which sites touch it. |
| `main+0x14281` bit 3 | the screen fog grid is current — already documented (`terrain-depth.md`: "if `LosType & 8` clear, first rebuild the screen fog grid"). **New here:** the camera stepper clears it at `0x41CB6B` on every frame the eye and target disagree, so a stale target becomes a per-frame grid rebuild. Also cleared at `0x41CB3B`, `0x41C567`, `0x41CE0D`. |
| `main+0x2C76` / `+0x2C7A` | the engine's mouse position, two **DWORDs** — agrees with `ui-markers.md`. Worth restating because `+0x2C78` looks like the y and is the high half of x; and `main+0x2C74` is an unrelated word (the battleroom lock bit, `cmdline-options.md`). |
| `main+0x37E1F` / `+0x37E23` | screen width / height — the fields `vpwide` derives the true viewport rect from, and the ones the scroll poll compares against |

### Two per-cell loops that differ, and it matters

An off-map eye is only safe where the engine bounds-checks. Both of these read map arrays from
the eye, and they do not agree:

- **`0x4843C0`** (screen fog-grid rebuild, which `terrown` calls itself) tests each cell against
  the LOS map dimensions with **unsigned** compares — `cmp`/`jae` at `0x4844B9` and `0x4844C7` —
  so a negative index is *skipped*, not faulted. This is why the widened camera range needed no
  guard here.
- **`0x483FA0`** (the terrain pass) indexes the tile map at `main+0x1428B` with **no bounds
  check at all**: `0x48409B` does `imul` row × stride, `add` col, `lea ebp,[edx+eax*2]`. A
  negative eye would read before the array. Moot in practice — `terrown` skips the whole
  function — but it is the reason to keep the eye's excursion a property of *our* passes.

## Hard-coded limits & constants

[VERIFIED unless noted — from `EngineLimits.cpp`/`.h` and `tamem.h`]

- **Projectiles: 300** (`STOCK_PROJECTILE_LIMIT`). TADR raises to 3000.
- **Explosions: 300** (`STOCK_EXPLOSION_LIMIT`). TADR raises to 3000.
- **Model effects: 100**, backed by a **0x186A0-byte (100,000) pool**. TADR → 1000.
- **Players: 10.** Structural, not a tunable: `PlayerStruct Players[10]` is inline in
  `TAdynmemStruct` and every loop in TADR is `for (n = 0; n < 10; ++n)`. Raising this
  means relocating the array *and* every consumer.
- **Weapons per unit: exactly 3** — three pointer slots in `UnitDefStruct`. Also
  structural.
- **Unit limit: not a compile-time constant.** It is *live state*:
  `TAdynmemStruct::ActualUnitLimit` (offset `0x37EEA`) and `MaxUnitNumberPerPlayer`
  (`0x37EEC`), both **`unsigned short`** — hence the widely quoted 6553 ceiling in the
  ini. 250 is the retail *default*, not a hard cap.
- Network dropout timeout: default 30 s, range [30, 300], settable with the `-T`
  command-line flag.
- **The real ceiling is the 32-bit address space**, and it bites: TADR's
  `AreaDamageOverflow.cpp` notes a damage grid at "16 bytes/cell — 4.8 MB on a 418x712
  map, 16 MB on 1024x1024", and `EngineLimits.cpp` had to hand-write a **stack-probe
  thunk** because expanding the projectile cleanup arrays to 0x2EF0 bytes overflowed
  more than one guard page.

## Data loading (archives, override order)

- **HPI format is fully decoded** [VERIFIED]: 20-byte header, magic `"HAPI"`, version
  `0x00010000`, key at 0x0C, directory offset at 0x10. Whole-archive XOR obfuscation
  keyed by `transformed = (((key >> 6) | (key << 2)) & 0xFF) ^ 0xFF` from offset 0x14.
  9-byte directory entries (name offset, data offset, flags; bit0 = directory,
  bit1 = SQSH-compressed). Payloads chunked at 0x10000. SQSH modes: 0 = stored,
  1 = custom 12-bit LZ77 (0x1000 ring buffer, mirroring `fcn.004D35F0`), 2 = zlib.
  `ioma8/totala-re`'s `hpi_assembler.py` round-trips archives byte-identically.
- **Archives and real directories are equivalent.** From the community TA Design
  Guide: "Total Annihilation sees the contents of the compressed files exactly as it
  does the actual file system directory structure."
  ([mirror](https://downloads.ta3d.org/misc/tadesign%20guide/tadesign/ta-files.htm))
  The engine runs a **virtual filesystem overlay**, which is why loose files in
  `units/` override archived ones. [VERIFIED]
- **The resolver is a single choke point.** TADR hooks exactly two addresses to see
  every asset request: **`0x004BB332`** (path satisfied on the native filesystem) and
  **`0x004BB3A7`** (path satisfied from inside an `.hpi`/`.ccx`/`.ufo`/`.gp3`
  archive); the archive's own filename sits at `[eax + 0x14]`. Four extensions are
  handled: `.hpi`, `.ufo`, `.ccx`, `.gp3`. The engine's hardcoded top-level directory
  names live in `.data`: `units` `0x503920`, `weapons` `0x50392C`, `features`
  `0x502C7C`, `scripts` `0x503740`. [VERIFIED]
- **Whether the loader is table-driven — and thus whether a *new* archive type could
  be registered — is NOT established.** Nobody has published the mount/enumeration
  routine. The precise **override precedence order between archive types is also
  unverified**: the definitive TAU thread on the subject
  (`tauniverse.com/forum/showthread.php?t=39737`, "Requesting help about hpi, ccx,
  gp3, ufo priorities") is behind Cloudflare and could not be read. Do not trust any
  ordering rule you have not tested. [GAP]
- TA registers itself with DirectPlay under the exe name `totala.exe`
  (`ta-forever/gpgnet4ta`). [VERIFIED]

## What is genuinely known vs. folklore

**Genuinely known** — machine-checked or self-validating: PE anatomy and imports; the
0x400C00 VA↔file delta; ~470 code addresses and ~86 named functions; the
`static_assert`-guarded struct layouts; the 0x00511DE8 global root; the command-table
ABI, `InitInternalCommand` (0x4B7760) and `IsCheating` (0x5091CC); the 300/300/100
effect-pool limits; the HPI/SQSH format.

**Folklore, or unverified** — the 0x90A50/0x90A60 unit-cap offsets (repeated
everywhere, never with a disassembly justification); the semantics of most `+debug`
commands; the "5000 unit patch" family, circulating as prebuilt binaries with no
published byte tables; the ini keys' availability on retail v3.1; all archive
precedence claims.

**Heuristic:** a claim tracing to TADR source is testable and probably right — the code
refuses to apply a patch whose `expected` bytes don't match. A forum post quoting a
file offset with no disassembly should be verified before you build on it.

## Gaps: what nobody has published

1. **No Ghidra/IDA project, symbol map, or `.idb` export exists publicly.** The
   knowledge is embedded in TADR's C++ headers and comments — readable, but not
   loadable into a disassembler. Reconstructing a symbol table from
   `HardCodeFunctions.cpp` + `tamem.h` would be a genuinely valuable, and quite
   mechanical, first contribution.
2. **The archive mount routine and override precedence.** The highest-value unknown
   for a modding project, and it is blocked partly by TAU being unreachable.
3. **The command table's own base address and length.** We know entries are
   `{name, fn, level}`, we know one entry's run-level byte is at `0x501DF4`, and we
   know how to *append* a table — but the built-in table's start address and entry
   count are not published, so the full command list cannot be enumerated from the
   table itself (only from a strings dump).
4. **The pathfinder.** `AISearchMapEntries` is tunable and TADR notes COB `XY_HYPOT`
   breaking on large maps, but no structural map of pathfinding exists.
5. **TA:CC vs. base-game executable differences** — not established.
6. **The simulation/network determinism model.** Packet IDs and lengths are partially
   documented in TADR's `src/Docs/` (some of it in Swedish), but the lockstep model is
   not written up.
7. **Teleport support is present but dormant.** TADR's `ta info.txt` records
   `UnitDefStruct.UnitBitfields (0x241) bit 0x2000 = Is Teleporter` and a
   `Cursor_Teleport` field, with the note "track down how to enable teleporter code" —
   an unfinished Cavedog feature still sitting in the shipped binary.

## Sources

- TA Demo Recorder (TADR) — <https://github.com/tanvanman/TADR> — primary source for
  nearly all addresses and structures. Mirror: <https://github.com/Skirmisher/tadr>
- `ioma8/totala-re` — <https://github.com/ioma8/totala-re> — HPI/SQSH format,
  radare2 startup analysis
- Hybrid Analysis PE report for `totala.exe` —
  <https://hybrid-analysis.com/sample/c94faf30b8a15a2f92f885c6f0f972cbadda765b4a920f87977e90f0e5538048/5f4a4258402fb1097172bb74>
- Command/cheat strings dump — <https://the-spoiler.com/STRATEGY/Cavedog/total.annihilation.2.html>
- TA Design Guide, "TA Files" (archives ≡ directories) —
  <https://downloads.ta3d.org/misc/tadesign%20guide/tadesign/ta-files.htm>
- Unit cap hex offsets & `TA.ini` keys (Steam discussions) —
  <https://steamcommunity.com/app/298030/discussions/0/597405278050241107/> and
  <https://steamcommunity.com/app/298030/discussions/0/1842367319524387998/>
- TA Forever launcher — <https://github.com/ta-forever/gpgnet4ta>
- `Skirmisher/TA-Patch-Installers` — <https://github.com/Skirmisher/TA-Patch-Installers>
  (patch inventory only; contains **no** offsets)
- Unofficial patch v3.9.02 — <https://archive.org/details/ta3902b_patch>
- Empty/abandoned: <https://github.com/tareversing/tareversing>,
  <https://github.com/cjg38340/TotalA_exe-Rewrite>

**Unreachable during this research** (Cloudflare / blocked): all of `tauniverse.com`
including thread 45399 "Hacking TotalA.exe", thread 44587 "A small Hacking tutorial
section", thread 39737 "hpi, ccx, gp3, ufo priorities", and `files.tauniverse.com`;
`web.archive.org`; `totalannihilation.fandom.com` (HTTP 402);
`units.tauniverse.com`. A researcher with browser access to TAUniverse should re-run
this survey — it is the single biggest blind spot here.
