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
| `main+0x2C76` / `+0x2C7A` | the engine's mouse position, two **DWORDs**, in **SCREEN** space (measured 2026-09-03: an injected pointer at screen (400,300) reads back 400 / 300; `0x498DA0` is what makes the world point). Worth restating because `+0x2C78` looks like the y and is the high half of x; and `main+0x2C74` is an unrelated word (the battleroom lock bit, `cmdline-options.md`). |
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

## The blend LUT and the marker composites — mapped by us

[MEASURED 2026-09-03, this project — disassembly of the pristine Steam build plus live reads.
Established while fixing the waypoint star, which rendered teal because the engine's alpha
composite was blending it against our fill key (`ui-markers.md` §6). Every VA below was read
off `objdump` in this session unless a row says otherwise.]

**The graphics globals block.** `0x4B6220` is the whole accessor: `mov eax,ds:0x51FBD0; ret`.
(`0x4B6230` starts with the same load and is **not** a second accessor — it does
`or byte [eax+0xF1],0x8` and continues into a longer routine gated on `[eax+0xF0]` bit 1. A
mutator; do not call it for the pointer.) `0x51FBD0` sits past `.data`'s raw end in the BSS
region, mapped at run time — the same region as `TA_MAINPP 0x511DE8`. Live read: `*0x51FBD0`
= `0x0051F320`, so the block itself is static and only its contents move.

| Field | What it is |
| --- | --- |
| **`[globals+0xC0]`** | **Pointer to the 64 KB blend LUT.** Not a borrowable pointer — the block is *owned* (allocated, freed and refilled; see below). Live read: `0x02C91D68`, i.e. heap. |
| `[globals+0xF0]` bit 5 | Gates every LUT user: `test cl,0x20` at `0x4B8519`, and `shr cl,5; test cl,1` at `0x4BAADB` / `0x4BA765`. Name *[INFERRED]* — "translucency available". |

**The LUT's shape.** `out = tab[(src << 8) | dst]`, so it is 256 rows of 256, indexed
source-major. The inner loop is `0x4CBF99..0x4CBFAC`:

```
edx = [ebp+0x1C]        ; the table, arg 6
al  = [esi]             ; source texel
cmp al,[ebp+0x18]       ; the sprite's transparent index
je  skip                ;   -> leave the destination alone
ebx = eax; shl ebx,8    ; src << 8
al  = [edi]             ; DESTINATION texel  <-- this is what made the star teal
add ebx,edx
al  = [eax+ebx]         ; tab[(src<<8) + dst]
[edi] = al
```

Only **three call sites read the LUT** at all: `0x4CBF2C` from `0x4B8499` and `0x4B8682`, and
`0x4CC057` from `0x4B86AA`. `0x4B8682`/`0x4B86AA` are inside `0x4B8500`; `0x4B8499` is in a
neighbouring function that takes its table from its own argument and never reads
`[globals+0xC0]`.

**The LUT's lifetime — the reason a pointer of ours must never outlive one call.**

| VA | What it does to `[globals+0xC0]` |
| --- | --- |
| `0x4BA5C0` | **Allocates it**: `push 0x10000; push 0x50A430` (a tag string); `call 0x4D83B0` (TA's allocator); `mov [ecx+0xC0],eax`. |
| `0x4BA5F0` | **Frees it**: `mov ecx,[eax+0xC0]; push ecx; call 0x4D85A0` (TA's free). |
| `0x4BAAD0` | **Overwrites it wholesale**: `mov edi,[eax+0xC0]; mov ecx,0x4000; rep movsd` — 64 KB copied *in*. |
| `0x4BA750` | Builds its contents (0x510 bytes of stack scratch; same bit-5 gate). |

So the slot is a heap buffer with a lifetime, not a hook point. Leaving our own table's address
there across a frame would let a reload `rep movsd` 64 KB into *our* buffer — silently undoing
the fix while leaving the engine's real table stale for every other blend in the session — or
let the teardown hand a DLL-heap block to TA's static-CRT free. `0x4D83B0` / `0x4D85A0` are
TA's own malloc/free.

**The composite, and who reaches it.** `AlphaCompsteBuf2OFFScreen 0x4B8500` re-reads
`[globals+0xC0]` *inside* the call, at `0x4B8665` and `0x4B8691`, which is what makes a swap
bracketed around the call visible to it. It has **24 call sites**: `0x4399BB`, `0x459319`,
`0x459353`, `0x4593BA`, `0x4595E9`, `0x4597D3`, `0x46A7AF`, `0x46A807`, `0x46A840`, `0x4736B1`,
`0x474291`, `0x474CA3`, `0x475080`, `0x4755C6`, `0x475757`, `0x49C0F5`, `0x49C24A`, `0x49C2DC`,
`0x49C40E`, `0x49C46F`, `0x4B7FFE`, `0x4B81BE`, `0x4B838A`, `0x4B8579`. All 24 accounted for,
because this survey is the evidence that a bracketed swap cannot be observed by anything else:

| Sites | Where | Inside the capture window? |
| --- | --- | --- |
| `0x4399BB` | the target sprite `0x439740` | **yes, always** — this is the one we wrap |
| `0x4B7FFE` | `CopyGafToContext`'s composite branch, taken when a sub-frame's `+0xB` is non-zero | **yes, but GAF-data-gated** — the route dots; stock `pathicon` frames do not take it |
| `0x459319`, `0x459353`, `0x4593BA`, `0x4595E9`, `0x4597D3` | the unit row sweep | no — earlier in the frame |
| `0x4736B1`, `0x474291`, `0x474CA3`, `0x475080`, `0x4755C6`, `0x475757` | the effect-object handlers, reached through `0x471F90`'s indirect `call [edx+8]` at `0x471FBB` (vtable `0x4FD638` slot 8 = `0x475700`) | no — but note **both hooks call `0x471F90` themselves**: hook 8 draws layer 8 *before* opening the window and hook 9 draws layer 9 *after* closing it, so they sit outside it by ordering rather than by address. That ordering is load-bearing; see `tagpu_markown.c`. |
| `0x49C0F5`, `0x49C24A`, `0x49C2DC`, `0x49C40E`, `0x49C46F` | the projectile pass `0x49BE60`, called at `0x469B22` | no — before hook 8 |
| `0x46A7AF`, `0x46A807`, `0x46A840` | past `DrawGameScreen`'s `ret` at `0x46A3FD` | no — different function |
| `0x4B81BE`, `0x4B838A`, `0x4B8579` | inside the `0x4B8xxx` composite family itself (self/sibling recursion) | only as children of a site above |

| VA | What it is |
| --- | --- |
| **`0x439740`** | **The order pass's target sprite** — the pulsing star at a move/attack waypoint, and the only alpha-composited marker. `stdcall(ctx, view, node, pos, flag)`, `ret 0x14`. Two direct `E8` callers: `0x439516` (inside the route-dot drawer `0x4394E0`) and `0x439C7D` (the walker `0x439B30`'s bit-3 dispatch). Its address is **also** in `.rdata` 19 times as the `+8` field of the 25-byte order-descriptor records behind `*(u32*)0x512344` — first occurrences `0x4FC4B1`, then `0x4FC754`/`0x4FC76D`/`0x4FC786`/`0x4FC79F` at stride 25. That field is reported unread in this build (the dispatcher loading only `+0`, `+4`, `+0xC`, `+0x10`, `+0x14`, `+0x15`) — *that* half is [FROM REVIEW, not re-derived here]; the 19 records and the stride are measured. If it were ever brought into use, a wrapper on the two `E8` sites would be bypassed silently. |
| `0x4B7F90` | `CopyGafToContext` — the route dots' blitter, and **usually** a masked copy. But `0x4B7FF7` reads each sub-frame's byte at `+0xB` and `jbe`-skips only when it is zero: non-zero routes into `0x4B8500` at `0x4B7FFE`. So whether the dots blend is a property of the **GAF data**, not of the code. Stock `pathicon` frames do not carry it, which is why they render solid. |
| `0x4BF8C0` | `DrawTranspRectangle` — named for its hollow centre, **not** for translucency. Clips through `0x4C5E70` and draws edge runs via `0x4BEA20`; it reaches no alpha composite. Corrects a long-standing claim in `ui-markers.md` and `tagpu_markown.h` that its "transparent edges" read the destination. [The store-only inner writer `0x4CC7AB` is FROM REVIEW; independently confirmed on screen — the drag band box renders as a clean white outline instead of washing out to teal the way the star did.] |
| `0x4C14F0` | `DrawTextCustomFont` (the group digits, drawn inside the same window). Calls `0x4B6220`, `0x4B6750`, `0x4C5E70`, `0x4C5FA0`, `0x4C6AE0`, `0x4CCF60`, `0x4E4760` — it blits through `0x4CCF60` and never touches the LUT, so an identity table cannot affect it. |

### `DrawGameScreen 0x468CF0` — its arguments, and the branch that skips hook 9

Prologue `sub esp,0x214` then four pushes (`ebx`, `ebp`, `esi`, `edi`), so inside the function
argument 1 is at `[esp+0x228]` — which is the `ebx` the marker block tests. Signature confirmed
as `(drawUnits, blitScreen)` stdcall.

**`0x469C01 test ebx,ebx / 0x469C03 je 0x469D38` jumps PAST hook 9 at `0x469D2C`.** A
`drawUnits == 0` frame therefore runs hook 8 and never reaches hook 9 — and the order markers
are drawn at `0x469BFC`, *before* that branch, so such a frame does reach the sprite. Any
state a capture opens at hook 8 must be able to survive not being closed.

| Caller | Passes | Which is it |
| --- | --- | --- |
| `0x495C76` | `drawUnits=1`, `blitScreen=0` | literal `push 0x1` |
| `0x495E66` | `1`, `1` | literal |
| **`0x4962C2`** | **`drawUnits=ebx=0`**, `1` | **TA's movie recorder.** Function starts `0x495E88`, `xor ebx,ebx` at `0x495EA1` and no other write to `ebx` before the call. Strings: `"%s\\MOVIE%03i"` `0x509500`, `"%s\\MOVIE*"` `0x509510`, `"FRAM"` `0x5094F8`. |
| `0x4969CD` | `1`, `1` | the in-game frame callback `0x496790`, which sets `mov ebx,1` at `0x4967CF` and uses `ebx` as its constant 1 throughout. |

Live counter-check on the played path: hook-8 opens and hook-9 closes were equal across
~50 000 blocks of a skirmish, so nothing in normal play takes the `drawUnits == 0` route.

### `KeyboardHotkeySampler 0x4C1B80` — why polling it twice is safe

`ui-markers.md` relies on this and it is worth having in the map. The function is a jump table
(`0x4C1C48`, index bytes at `0x4C1C6C`); the SHIFT entry `0xF9` lands at `0x4C1BB5`, which is
`push 0x10; call ds:0x4FC350` (`GetAsyncKeyState`) then **`and al,0xfe`** — masking off bit 0,
the "pressed since last call" bit — before `neg ax; sbb eax,eax; neg eax` normalises to 0/1.
All nine stubs in the `0x4C1BA1..0x4C1D56` block do the same mask, so nothing in the engine
reads the consumable bit and an extra poll of our own cannot steal an edge.

## The unit blit's shadow branches — mapped by us

[MEASURED 2026-09-03, this project — `objdump` of the pristine Steam build, plus the live A/B in
`shadows-cloak.md` §"Structure shadows, owned". Established while fixing the teal structure
shadows (G13k): the engine's cached slant shadow is drawn through the ALP blend blit and,
inside a key-filled viewport, blends against the fill key. Every VA below was read off the
disassembly in this session.]

**`0x459200` — the per-unit composite blit** (`ret 0x18`), reached from `0x458810` under
`DrawUnit 0x45AC20`. It splits on the composite's depth-plane pointer at `0x45927E..0x459282`
(`mov eax,[esi+0x14]; test eax,eax; jne 0x45949D`): **path A** (colour-only composite) at
`0x459288`, **path B** (colour + depth) at `0x45949D`. Both open with the same shadow decision
tree, drawn before the body. Every site in it:

| Path A | Path B | What it is |
| --- | --- | --- |
| `0x45928E` `mov ax,[ecx+0x37F06]` | `0x4594A2` | the graphics-option word; `test al,4` (Shadow) right after, `je` to the body |
| `0x4592A0..0x4592AC` | `0x4594B4..0x4594C0` | `unit+0x92 → UnitDefStruct`, `+0x241` type mask, `test …,0x2000000` = `noshadow` → skip |
| `0x4592BF` `test byte [ecx+0x113],0x20` | `0x459522` `test dword [ecx+0x110],0x20000000` | **the structure bit** of `unit+0x110` |
| **`0x4592C6`** `74 5C` `je 0x459324` | **`0x45952C`** `74 4A` `je 0x459578` | not a structure → the COMPLETED branch. **`owndraw all` rewrites both to `EB` (`jmp`)** — `tagpu_owndraw.c`, verified byte-for-byte before the write, installed as a pair or not at all |
| `0x4592C8` `test [esp+0x10],0x40000000` | `0x4594D0` `shr edx,0x1E; test dl,1` | `digger` → the COMPLETED branch too (path B clips it below ground first, `0x4594DB..0x459503`) |
| `0x4592D5` `cmp word [eax+0xA6],0` | `0x45952E` | `unit+0xA6` — the **model index** (`U_MODELID` in the native pass), not a unit id. Zero → |
| `0x4592E4` `movzx cx,byte [eax+0x1427F]` | `0x45953D` | sea level; `cmp word [esp+0x42],cx ; jl` skips the shadow for a model-0 unit below it |
| `0x4592FE` `call 0x45A790` | `0x45955B` | build the cached slant shadow when `Object3do+0x14` is NULL |
| **`0x459319`** `call 0x4B8500` | `0x459576` `jmp 0x4595E9` → **`0x4595E9`** | blit the cached shadow, at `sx + 0x85` (`add edx,0x85` at `0x45930B` / `0x45956C`). Path B shares one call site between the digger, structure and completed branches; path A has one per branch |
| `0x459324` `shr al,3; test al,1` | `0x459578` | the COMPLETED branch: TShadow bit, then `test …,0x81000` (`canhover`/`floater`), `0x45A470` (scratch := composite silhouette), blit at `0x459353` / `0x4595E9` |
| `0x4593BA` `call 0x4B8500` | `0x4597D3` | the body blit, further down each path |

So the five "unit row sweep" call sites of `0x4B8500` in the blend-LUT survey above are:
`0x459319` structure shadow (A), `0x459353` completed shadow (A), `0x4593BA` body (A),
`0x4595E9` every shadow (B), `0x4597D3` body (B).

**The slant builders.** `0x45A510` (ground-projection AABB) walks the prims at stride `0x36`
from `Object3do+0x22` and tests only flag bit0 (`test byte [ecx+0x28],1` at `0x45A55B`).
`0x45A610` (the silhouette raster) tests **bit0 and bit1** (`test cl,1; je` at `0x45A64C`,
`test cl,2; je` at `0x45A655`) and flat-fills each face through `0x4C1000` at `0x45A750`. The
projection is `gx = x + y/4`, `gy = −z − y/4` against the body's `−z − y/2`, so a point at
height `y` casts `y/4` right of and `y/4` below its drawn position. Bit1's meaning is open:
a completed CORE wind generator's mast and rotor lack it (they cast nothing in the engine)
while its base pieces carry it.

**Why the branch flip is safe.** Both `je`s are 2-byte short jumps whose fall-through and
target both continue with `eax` still holding the option word the target tests (`shr al,3`),
and the COMPLETED branch is the engine's own path for every non-structure unit; under
`owndraw all` its composite is blank, so `0x45A470` builds an all-key silhouette and the blit
writes nothing. Nothing else reads `Object3do+0x14`; it is simply never allocated. Without
`all` the bytes are left alone and structures keep the engine's cached shadow.

**Negative results.** `[esp+0x42]` is compared with sea level but was not traced back to its
producer (`[esp+0x14]` is the altitude the waterline code subtracts; `+0x42` is a different
word). The body punch-out `0x4B9D70(body, scratch, 5, 0)` inside `0x45A790` was read, not
replicated. `0x459200` itself was not disassembled past `0x459900`.

## The cursor chain — mapped by us

[MEASURED 2026-09-03, this project — disassembly of the pristine Steam build (`objdump -d -M
intel`) plus live `tacli peek` reads on a running game. Established while fixing "selecting a
unit does not give the move cursor" (`field-notes.md`, our patch 2). Every VA below was read
off `objdump` in this session unless a row says otherwise.]

**The chain, top to bottom.**

| VA | What it is | How established |
| --- | --- | --- |
| `0x490B30` | `SetInputMode(mode)` *[INFERRED]*. Stores `mode` at `main+0x391F1` and installs the matching per-event handler into `main+0x391F5`, off a jump table at `0x490C14` (modes 0–7). 11 call sites. **Mode 6 is in-game** — the value a dozen sites elsewhere test for (`0x4289F1`, `0x45053A`, `0x476C75`, `0x476CA5`, `0x476E5B`, `0x478DB6`) | disassembly |
| `0x499200` | the **mode-6 (in-game) mouse handler**: `rep movsd` six dwords from `main+0x2C76` onto the stack, `call 0x498DA0`, then choose and set the cursor. **No `call` site anywhere**: the 4-byte literal `0x00499200` occurs in the image only as the immediate of the two `mov dword [main+0x391F5],0x499200` stores at `0x490BC5` (mode 6’s arm) and `0x498455` | disassembly + a scan for the literal across all sections |
| `0x498DA0` | mouse → world point, map cell and hovered feature. One call site, `0x499221` — the one `vpwide` redirects (`gpu-status.md` §2.3b) | disassembly |
| `0x48CD80` | the unit-under-cursor lookup *[INFERRED]*; its return is stored as a WORD to `main+0x2CBA` at `0x499283`. **Not disassembled** — known only by that call site and by the field's live behaviour | call site + live read |
| `0x48D220` | `CorretCursor_InGame(orderByte)` — returns the cursor index to show. Two call sites: `0x491D36` and `0x499297`. `ret 4` | name [CORPUS] (TADR, `tools/ta_symbols.txt`); body from disassembly |
| `0x43E490` | the per-candidate cursor mapper: `(orderByte, candidate, hoveredUnit, &worldPos)` → cursor index, `ret 0x10`. **Exactly one caller, `0x48D3E4`**, and the literal `0x0043E490` appears nowhere in the image — no dispatch table, no indirect call | disassembly + literal scan |
| `0x4AB400` | `SetUICursor(uiCtx, gafSequence)` — 12 call sites, of which **five are inside `0x499200`**: `0x4992C8` (the cursor block below) plus `0x499560`, `0x4995A3`, `0x49966D`, `0x4997F7` in its button handling | name [CORPUS]; call sites from disassembly |

**What `0x499200` does, and the gate that decides whether a cursor is chosen at all.** This is
the head of the handler, not the whole of it: there is no `ret` between `0x499200` and the `c3`
at `0x499876`, so `done` below means "jump to `0x4992CD`", where the same function carries on
into `call 0x41C180`, the `main+0x2CC6 & 0x20` test at `0x4992D8` and the mouse-button handling
— which is where two of the four ordering readers of `main+0x37EFA` (`0x499352`, `0x499567`)
live. *[Extent FROM REVIEW 2026-09-03; the block below was first written as though each `done`
returned.]*

```
edx  = main                       ; ds:0x511DE8
copy 6 dwords main+0x2C76 -> stack ; the mouse POINT and four more dwords
call 0x498DA0(&copy)              ; fills the world point / cell / hovered feature
cl   = main[0x2CC6]
if ((cl & 2) && main[0x2CC3] == 0x0E) { call 0x4197D0; done }   ; build placement mode
if (!(cl & 2) && !(cl & 1)) {                                  ; pointer on NEITHER region
    if (main[0x2CBE] != 0x13) { main[0x2CBE] = 0x13;
                                SetUICursor(main+0x519, *(main+0x148CB)); }   ; cursornormal
    done
}
main[0x2CBA] = (WORD)0x48CD80()                       ; the unit under the cursor
eax = CorretCursor_InGame(main[0x2CC3])               ; the index it wants
if (eax != main[0x2CBE]) { main[0x2CBE] = eax;
                           SetUICursor(main+0x519, *(main+0x1487F + eax*4)); }
```

Two things fall out. The store-if-changed guard at `0x4992A2` is the address TADR's corpus
names `SuppressShowReclaimCursorAddr` — independent corroboration that `main+0x2CBE` is the
current cursor index. And `*(main+0x148CB)` is `cursor_ary[0x13]`, which the loader table below
independently makes `cursornormal`: the off-region default and index 19 agree.

**`0x498DA0` and the mouse-region bits of `main+0x2CC6`.** The function tests the pointer against
two rects with `0x4B6720` (point-in-rect *[INFERRED]*) and records which one it landed in:

| bit | Meaning | Where |
| --- | --- | --- |
| `0x01` | pointer is inside the rect at `main+0x142BB` — the minimap's click area *[INFERRED]*: it is the rect this arm scales the pointer by the map size against, and it sits immediately below the minimap view RECT at `main+0x142CB` (`gpu-status.md` §2.5) | set `or bl,1` at `0x498DE4`; cleared `and bl,0xFE` at `0x498E8E`. **Disassembly only — not confirmed live** |
| `0x02` | pointer is inside the **world viewport** rect `main+0x37E27` | cleared `and …,0xFD` at `0x498E26` on the minimap path; set from `PtInRect` at `0x498EAD..0x498EBC`. Live: reads **6** with the pointer anywhere on the world, **0** on the lower side panel |
| `0x04` | set when **bit 0 or bit 1** is — "the pointer is on one of them" | computed at `0x498ECE..0x498EE6` |
| `0x08` | already documented elsewhere; gates the minimap branch at `0x498DD5`: while it is set the minimap rect is not consulted at all. `ui-markers.md` §4 has it as the drag/band "rect forced on" bit; the two readings are consistent | disassembly |

The minimap arm scales the pointer by the minimap rect (`main+0x142E7..0x142ED`) against the map
size (`main+0x1422B`/`+0x1422F`); the viewport arm is the `world = eye + clamp(pos, L, R) − L`
form already documented in `gpu-status.md` §2.3b. The tail converts the world point to a cell
pair at `main+0x2C8E` (`>> 0x14`) and stores the **hovered feature id** as a WORD at
`main+0x2CBC` — live: `0xFFFF` over open ground, `87` over a lab wreck.

**`CorretCursor_InGame 0x48D220(orderByte)`.** It gathers candidates, maps each one and keeps
the lowest index:

- hovered unit: `main+0x2CBA` != 0 → `unit = *(main+0x14357) + id*0x118` (`0x48D242`: `id*8 − id`,
  then `*5`, then `*8` — 280 = `0x118`, the same unit stride `gpu-status.md` §2.5 records);
- the local player's selected units: `esi = main + 331*p + 0x1B63` where `p = main[0x2A42]`
  (`0x48D280..0x48D292`: `p + (33p)*5*2 + 0x1B63`), then walk `[esi+0x67] .. [esi+0x6B]` in
  `0x118` steps, calling `0x48DDC0` for each entry whose `[unit+0x110]` has bit 4 set. The
  begin/end pair is the same `PlayerStruct+0x67/+0x6B` `ui-markers.md`'s appendix records for
  the order-marker walk; bit 4 meaning "selected" is *[INFERRED]* from the use;
- the hovered unit adds its own candidate through `0x480100`;
- then `for each candidate: idx = min(idx, 0x43E490(order, cand, hovered, main+0x2CAA))`,
  starting from `0x13`. **With no candidates at all** it returns `0x0F` when the hovered unit is
  the player's own and passes four field tests (`+0xFF == player`, `+0x110 & 0x20`,
  `+0x104 < *0x4FD758`, `+0xFB == 0`), else `0x13`.

**The order → case jump table at `0x43F0A8`.** `0x43E490` dispatches on `orderByte − 1`, `ja
0x43F098` above 13, so **only order bytes 1..14 have cases**:

| order | case VA | order | case VA |
| --- | --- | --- | --- |
| 1 | `0x43E505` | 8 | `0x43E5FA` |
| 2 | `0x43E8BB` | 9 | `0x43E5DE` |
| 3 | `0x43E545` | 10 | `0x43F098` (the default) |
| 4 | `0x43E850` | 11 | `0x43E8AE` |
| 5 | `0x43E80C` | 12 | `0x43E65C` |
| 6 | `0x43E7D3` | 13 | `0x43E797` |
| 7 | `0x43E615` | 14 | `0x43E828` |

`main+0x2CC3` holds the current order byte. Measured live by clicking each order button and
reading it back: **1** = contextual (no command button pressed), **2** = Move, **3** = Attack,
**7** = Guard, **8** = Repair, **9** = Patrol, **12** = Reclaim, **13** = Capture, **14** =
build placement (the `0x0E` that `ui-markers.md` §4 already records as "build mode").

**The Interface Type gate — the bug.** Order 1's case opens with

```
0043E505  83 BB FA 7E 03 00 01   cmp dword [ebx+0x37EFA], 1     ; Interface Type
0043E50C  0F 84 F0 05 00 00      je  0x43EB02                   ; right-mouse-orders branch
```

and `0x43EB02` can only ever return `0x0F` `cursorselect`, `0x11` `cursorred`, `0x12`
`cursorgrn` or fall through to `0x13` `cursornormal`. The classic branch at `0x43E512` instead
re-dispatches: `[esp+0x18] = 3` at `0x43E520` and `= 0xC` at `0x43E53B` rewrite the order byte
and jump back to `0x43E49F`, so the contextual cursor becomes the **attack** and **reclaim**
cases, with the move case reached through `0x43EDB6`.

Measured on a **stock** instance (nothing armed), commander selected, on `shadow-mix`:

| pointer over | Interface Type 1 | Interface Type 0 |
| --- | --- | --- |
| empty ground | `0x13` `cursornormal` | `0x0E` `cursormove` |
| lab wreck | `0x12` `cursorgrn` | `0x0B` `cursorreclamate` |
| own building | `0x0F` `cursorselect` | `0x0F` `cursorselect` |

`main+0x37EFA` is the `Interface Type` registry value (already in `resolution.md`), read at
`0x42F9AF` and clamped to ≤ 1 at `0x42F9CD`. It is **read at nine sites**: `0x42F9F6` and `0x430F08` (the settings
round-trip), `0x45EF6D` (the options screen pushing the value into its `LEFTCLICK` gadget,
`0x4A1080("LEFTCLICK", value)`), `0x43E505` (this one, cursor only), `0x43F9EF`, and `0x499046`
/ `0x499162` / `0x499352` / `0x499567` — the last two inside `0x499200`'s own button handling.
Those four are where left-vs-right ordering lives *[INFERRED from their position in the input
path; not traced instruction by instruction]*.

**The licence for the patch is the one-caller fact above, not this enumeration.** `0x43E490`
contains no store to `main` at all and is reached from exactly one place, so neutering
`0x43E50C` changes which sprite is chosen and can reach nothing else — that holds however many
readers of `main+0x37EFA` turn out to exist. *[The ninth reader is FROM REVIEW 2026-09-03; the
list first said eight and the split was called "the whole licence", which overstated what an
enumeration can prove.]*

**`cursor_ary` — index → GAF sequence.** Base `main+0x1487F`, entry `idx*4`, matching
`ui-markers.md`'s `cursor_ary[0x15]` [CORPUS]. The mapping is read out of the loader at
`0x429C84..0x429E94`, which opens the `cursors` GAF (`0x429700`, handle to `main+0x14903`) and
then calls `0x4B8D40(handle, name)` once per sequence, storing **the previous call's** result —
so the name pushed at a site belongs to the offset stored at the *next* one:

| idx | name | idx | name | idx | name |
| --- | --- | --- | --- | --- | --- |
| 1 | `cursorattack` ✔ | 8 | `cursorpickup` | 15 | `cursorselect` ✔ |
| 2 | `cursorairstrike` | 9 | `cursorteleport` | 16 | `cursorfindsite` |
| 3 | `cursortoofar` | 10 | `cursorrevive` | 17 | `cursorred` |
| 4 | `cursorcapture` | 11 | `cursorreclamate` ✔ | 18 | `cursorgrn` ✔ |
| 5 | `cursordefend` ✔ | 12 | `cursorload` | 19 | `cursornormal` ✔ |
| 6 | `cursorrepair` | 13 | `cursorunload` | 20 | `cursorhourglass` |
| 7 | `cursorpatrol` ✔ | 14 | `cursormove` ✔ | 21 | `pathicon` |

Index **0** (`main+0x1487F` itself) is written by nothing in that loader and was not chased.
✔ marks the eight confirmed by behaviour rather than by the loader's instruction order alone —
seven by driving the game and reading `main+0x2CBE` back, and `cursormove` and
`cursorreclamate` additionally by reading their sprites out of the GL framebuffer. `pathicon` at
21 agrees with `ui-markers.md` §3, which already had the route dots at `main+0x148D3`.

**Globals this chain owns.**

| Field | What |
| --- | --- |
| `main+0x2CBA` | WORD, unit under the cursor (0 = none). Written at `0x499283` |
| `main+0x2CBC` | WORD, **feature under the cursor** (`0xFFFF` = none). Written at `0x498F5D` |
| `main+0x2CBE` | BYTE, **the cursor index currently installed**. Written at `0x49925D` / `0x4992AD` |
| `main+0x2CC3` | BYTE, current order byte (the jump-table selector above) |
| `main+0x2CC6` | BYTE, mouse-region flags — bits 0/1/2 above, 3/6 in `ui-markers.md` §4 |
| `main+0x2CAA` | the world point under the cursor, filled by `0x484B50` from inside `0x498DA0` |
| `main+0x2C8E` / `+0x2C90` | the map cell pair, `world >> 0x14` |
| `main+0x391F1` / `+0x391F5` | input mode, and the handler pointer for it |
| `main+0x14903` | the `cursors` GAF handle |
| `main+0x2A42` / `+0x2A43` | local player index, and the player's LOS bit (`shl 1, cl` at `0x43EBF4`) |

**Negative results worth the line.**

- **`0x43E490` has one caller and no address literal in the image.** That is what makes a patch
  there provably cursor-only, and it is the fact the whole fix rests on.
- **`0x499200` is never called.** Only installed, by mode 6. Anything hunting for "where the
  cursor is chosen" by following calls will not find it.
- **The handler is dispatched by `call dword [eax+0x391F5]` at `0x499A1C`.** Of the 24
  references to that displacement in `.text`, 23 are stores — the nine arms of `0x490B30`'s mode
  table plus `0x491617`, `0x49297E`, `0x492A7F`, `0x496AFF`, `0x496B94`, `0x496BF6`, `0x496C3D`,
  `0x496CA4`, `0x496D75`, `0x496D9D`, `0x496DFF`, `0x498457`, `0x4996DF`, `0x499852` — and that
  one is the read. *[FROM REVIEW 2026-09-03: this line first claimed there was no reader at all.
  It was written off a `grep | head -12`, so the twelfth match was the last one seen and the
  absence was an artefact of the pipe. Confirmed by re-running it unbounded.]*
- **Interface Type is not one switch with one meaning.** It gates the cursor at exactly one
  site and ordering at four others, and those are independent — which is why the game can be
  left on right-mouse orders and still show the classic cursors.
- **The contextual cursor is the only one the interface type touches.** Order bytes 2, 3, 9, 12
  … reach their cases without consulting `main+0x37EFA`, so the Move/Attack/Patrol/Reclaim/Guard
  buttons produced correct cursors at Interface Type 1 before the patch. Measured.

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
