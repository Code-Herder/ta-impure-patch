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
| Tile set / tile map | **`TILE_SET`** at `main+0x14283` → `{u32 count; u8* pixels}`: `count` 32×32 8bpp tiles of `0x400` bytes each, built by `LoadMap` and static for the map (Two Continents: 5062). **`TILE_MAP`** at `main+0x1428B`: `u16` tile index per 32-px cell, row stride `FeatureMapSizeX/2`. Both byte-confirmed against the terrain blit `0x483FA0` ([terrain & depth](terrain-depth.html) §2) and read every frame by `tagpu_terr.c`. The Classic++ restorer reads them once more per map, on the render thread, at the moment its job starts (`tagpu_terr.c` `glsl_begin`/`restore_order`, 2026-09-05): every `TILE_SET` tile's edge texels for the tileability test, and the whole `TILE_MAP` once to rank each tile by its distance in cells from the viewport (the restore runs visible tiles first). The pixels themselves are never copied again — the GLSL passes sample the R8 atlas the terrain pass already uploaded (the ONNX path that copied the whole set was deleted 2026-09-05). Read-only. |
| Live palette | `main+0x143A7`: 256 entries × 4 bytes, **R, G, B, pad** — and it does **not** cycle. The Classic++ restorer snapshots it once per job into its own 256×1 RGBA8 texture (the fill pass's palette lookup), so a restore is consistent with itself whatever the native pass uploads meanwhile. [MEASURED 2026-09-05] Read out of the live
process with `tacli peek '*0x511DE8+0x143A7:x256'` ×4 for the whole table: **all 1024 bytes
identical across 16 samples over 8 s** in a live skirmish, on two maps, with a further 24
samples of entries 0..63 over 10 s. This row previously called it "the palette the engine
cycles for water", which was never measured and is wrong; the pixel half of the test is in
[terrain & depth](terrain-depth.html) §7. Read every frame by `tagpu_native.c`/`tagpu_render3do.c` (uploaded verbatim as a 256×1 RGBA texture; the in-game colours match, which is the byte-order proof) and once per job by the restorer (snapshotted, above). Read-only. |
| Grey remap table (fog of war) | Built by **`0x4BAD30`** into `*(0x51FBD0) + 0xCC`, 256 bytes — the same object whose `+0xC0` is the blend LUT we swap (see the blend-LUT section); `0x4B6220` is a two-instruction accessor, `mov eax,ds:0x51FBD0; ret`. For each of the 256 palette entries it averages R, G, B — literally `(R+G+B)/3`, compiled as the `0xAAAAAAAB` multiply-high then `shr edx,1` — writes that one value into all three channels of a scratch triple and calls `0x4BA9D0` for the index it stores. `0x4BA9D0` is a nearest-colour search `[INFERRED name]`: it prefilters candidates to a **±0x28 band on the R+G+B sum** and carries a `0x3B9ACA00` (1e9) best-distance sentinel. The builder is gated on `[obj+0xF1] & 1`, and it reads the palette at `main+0x143A7`. **[VERIFIED by disassembly 2026-09-04]**, `0x4BAD30`–`0x4BADE0` and `0x4BA9D0`–`0x4BAA80`; the rest of `0x4BA9D0` was not read. This is the rule `TAGPU_GLSL_FOG_GREY_RGB` reproduces in RGB instead of through the palette, so Classic++'s restored terrain takes the same grey band. Siblings building the other tables from that palette: `0x4BA750`, `0x4BADF0`, `0x4BABD0`, `0x4BAF30` — the table set is mapped in [shadows and cloak](shadows-cloak.html). |
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
| `0x41CE90`…`0x41D060` | The scroll poll — see the table below. One caller, `0x496976`. *[CORRECTED 2026-09-04: this said `0x41CF10`, which is not an instruction boundary — `0x41CF0E` is `lea ebp,[esi+0x64]`.]* |
| `0x466B70` | Fills a RECT with the minimap's view box from the eye and the view size in map cells (`main+0x1423B`/`+0x1423F`). Pure computation; its only two call sites are inside `0x41C3C0`. |

**The scroll poll.** The position it tests comes from `[obj+0x196]` — the mouse object's own
record, fetched with `0x4C2340` at `0x41CEC5` — not from a fresh poll. *[CORRECTED 2026-09-04:
this said `GetCursorPos` (IAT slot `0x4FC2E0`). The poll at `0x41CEE7` is `GetCursorPos`
(`0x4FC2E4`) but serves only the off-screen warp-back below; `0x4FC2E0` is `GetFocus`.]*
Four independent directions, each firing on *hotkey* **or** *pointer on an exact screen
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
| `0x4BF8C0` | `DrawTranspRectangle` — named for its hollow centre, **not** for translucency. It opens by acquiring a drawing context through `0x4C5E70` (`ret 4`: calls `0x4B6220` for the graphics globals, and when `globals+0xDC` is set copies 0xC dwords from `globals+0xBC` into the caller's stack block and returns 1) and **abandons the whole draw when that returns 0** (`0x4BF8D7`, `je 0x4BFD49`); the companion release is `0x4C5FA0`. Then four edges, each clipped by `0x4BEA20` and written by the store-only `0x4CC7AB` — ten calls to each across the body — and it reaches no alpha composite. Corrects a long-standing claim in `ui-markers.md` and `tagpu_markown.h` that its "transparent edges" read the destination. [The store-only writer was FROM REVIEW and is now disassembled — see "the post-fog build cursor and band box" below, which also has the argument list, the edge split and the surface bound that clips it. The `0x4C5E70`/`0x4C5FA0` pair came from the aircraft-shadow landing's own read of this function and is kept here rather than lost to the merge.] |
| `0x4C14F0` | `DrawTextCustomFont` (the group digit and the range labels, drawn inside the same block). Calls `0x4B6220`, `0x4B6750`, `0x4C5E70`, `0x4C5FA0`, `0x4C6AE0`, `0x4CCF60`, `0x4E4760` — it blits through `0x4CCF60` and never touches the LUT, so an identity table cannot affect it. Full anatomy in §"The in-game bitmap font". |

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

### The sweep order inside `DrawGameScreen`, by call site

[BINARY-VERIFIED 2026-09-04 — an `objdump` of `0x468CF0..0x469C60` filtered for these four
targets, re-read for the aircraft work rather than taken from the earlier note.]

| VA | Calls | What |
| --- | --- | --- |
| `0x469920`, `0x46992F`, `0x469ABB` | `0x46A610` | features |
| `0x469A00` | `0x45AC20` DrawUnit | **site A** — ground units, `(state&3)==1`, per sort row |
| `0x469B22` | `0x49BE60` | weapons: laser lines and projectile GAFs |
| `0x469B2C` | `0x420B00` | explosions and effects |
| `0x469BA3` | `0x45AC20` DrawUnit | **site B** — everything `(state&3) != 1`, over ALL rows |

Site B is last, and a unit's shadow is blitted inside that same `DrawUnit` call (the branch
table above), through `0x4B8500`, which has **no depth test** — the ALP blit writes every
non-key pixel of its source. So an aircraft's ground shadow composites **above** the ground
units, the features, the projectiles and the explosions. That is the engine quirk
`shadows-cloak.md` §4 flags; it is settled by this ordering, and not by a screenshot — several
staged attempts to catch a shadow lying across a fireball never lined the two up.
### `0x469DB4..0x469F23` — the post-fog build cursor and band box, and why no capture can reach them

[MEASURED 2026-09-04, this project — disassembly of the pristine Steam build, plus a live A/B
at 1x, 2.144x and 0.467x. This is the block `tagpu_mark.c` now re-draws.]

The last world-anchored thing `DrawGameScreen` paints, after its fog overlay: one
double-outlined rectangle that is *either* the build-placement footprint *or* the drag band
box. One gate, one projection, two `DrawTranspRectangle` calls.

**The gate** (`ebx` is the `drawUnits` argument — `0x469DB4 test ebx,ebx`):

```
0x469DC2  test byte [main+0x2CC6],8   -> non-zero: draw  (the band box, forced on)
0x469DD8  cmp  byte [main+0x2CC3],0xE -> not 14: do not draw
0x469DE1  IsPositionInRect(main+0x37E27, main[0x2C76], main[0x2C7A])
0x469E0B  test eax,eax / je 0x469F30
```

`IsPositionInRect 0x4B6720` is `stdcall(RECT*, x, y) ret 0xC` and is **inclusive on all four
edges**: `x < r[0]` or `x > r[2]` or `y < r[1]` or `y > r[3]` returns 0, else 1. The rect it
reads is the viewport rect — the field `tagpu_vpwide` deliberately widens at zoom < 1, which
is why a placement out in the ring passes this gate at all.

**The projection** (`0x469E13..0x469E68`), all six source fields read as DWORDs:

```
l = main[0x2C92] - eyeX + 0x80              eyeX = main[0x1431F]
r = main[0x2C9E] - eyeX + 0x80              eyeY = main[0x14323]
t = main[0x2C9A] - (main[0x2C96] >> 1) - eyeY + 0x20      (SAR, signed)
b = main[0x2CA6] - (main[0x2CA2] >> 1) - eyeY + 0x20
if (r < l) swap;  if (b < t) swap           0x469E92 / 0x469EA0
```

`+0x80`/`+0x20` are **baked immediates** — the TRUE viewport origin, not a read of `L`/`T`.
So the block projects against the true origin while its gate tests the widened rect; that
combination is what makes the rect correct in the ring and is why `tagpu_vpwide`'s `0x498DA0`
stub redoes the mouse conversion with the true origin and the wide clamp.

**The colours.** `[esp+0x74]` is `main+0xDCB`, the GUI colour byte array — written once in the
prologue, `0x468D49 lea ebx,[eax+0xDCB]` / `0x468D51 mov [esp+0x74],ebx`. The outer index is
picked at `0x469E6B` from the MODE (not from which arm of the gate fired):

```
mode == 0xE:  and cl,0x40 / neg cl / sbb ecx,ecx / and ecx,6 / add ecx,4
              -> 0xA when main[0x2CC6] & 0x40 (site OK, green), else 4 (blocked)
otherwise:    0xF
```

and the inner rect, `{l+1, t+1, r-1, b-1}`, re-tests `main[0x2CC3]` at `0x469EF4`: build mode
reuses the same colour byte, anything else takes `gui[0]`. So build placement is a 2 px
single-colour outline and the band box is white over black.

**`DrawTranspRectangle 0x4BF8C0`** — `stdcall(ctx, RECT*, colour) ret 0xC`. Two paths on
`ctx == NULL` (`0x4BF8C6`): the null path fetches a global draw context through `0x4C5E70`
(which copies 12 dwords out of `[globals+0xBC]` or `globals+0x50`); the one `DrawGameScreen`
takes is `0x4BFC5F`, with the caller's context. Its four edges do **not** all go the same way:

| Edge | From | Via |
| --- | --- | --- |
| top `(l,t)-(r,t)` | `0x4BFC93` | `0x4BEA20` (clip) then `0x4CC7AB` directly |
| right `(r,t)-(r,b)` | `0x4BFCF5` | the same pair |
| bottom `(l,b)-(r,b)` | `0x4BFD2D` | `DrawLine 0x4BE950`, which does the same `0x4BEA20`+`0x4CC7AB` internally |
| left `(l,t)-(l,b)` | `0x4BFD40` | `DrawLine 0x4BE950` |

That asymmetry is a code-generation artefact, not a difference in output: all four reach the
same writer. `ui-markers.md` used to call two of them a "transparent line variant"; they are
the same line through one more frame. The two `test eax,eax / jne` on a `lea` of a stack slot
(`0x4BF905`, `0x4BF9CF`) are always taken, so the null-context arms below them are dead in
this build.

**`0x4CC7AB`** is `cdecl(ctx, x0, y0, x1, y1, colour)` and it is a **store-only Bresenham**:
`stos byte` with no read of the destination, so nothing here touches the blend LUT. Its two
axis-aligned special cases count **both endpoints** — `0x4CC83B` (dx == 0) does
`sub ecx,eax / inc ecx`, `0x4CC866` (dy == 0) the same — so a rect edge is inclusive at every
corner. `ctx+0x08` is the pitch and `ctx+0x0C` the pixel base, the same two fields
`tagpu_markown.c` swaps.

**THE NEGATIVE RESULT, and it is the load-bearing one.** `0x4CC7AB` opens by calling
**`0x4CC650`**, which reads `edi = [ctx+0x00]` and `esi = [ctx+0x04]` — the surface's WIDTH and
HEIGHT — and clips the line to `[0,w) x [0,h)`, returning 0 for a line wholly outside; the
caller then `je`s past the draw (`0x4CC7DD`). **The bound is the surface's own dimensions, read
below the clip rect at `+0x1C..+0x28`, so nothing done to the clip rect can widen it.** The
offscreen is screen-sized, and at zoom < 1 the rect above is projected at coordinates
`vpwide` made addressable — which run far past it. Measured on a 1024x768 frame at 0.467x:
with the pointer at screen (880,400) the engine's own mouse point is (1228,418), the six rect
globals are populated and `main[0x2CC3]` is 14, and **not one pixel is drawn**; at screen
(170,250) the engine point is (-294,97), same result. A capture window can therefore never
carry this rect into the outer ring, however wide its own buffer is — which is why it is
re-drawn instead (`tagpu_mark.c`, "THE BUILD CURSOR").

**And it NORMALISES BEFORE IT INSETS, which is not interchangeable.** `0x469E92`
(`cmp edi,esi / jge`) and `0x469EA0` (`cmp edx,eax / jge`) swap the x and y pairs, the swapped
values are what gets stored, and only then does `0x469ECA..0x469EDD` read them back and
`inc edi / inc esi / dec edx / dec ecx` for the inner rect. Insetting first and normalising
afterwards turns the inset into an OUTSET for any rect stored right-to-left or bottom-to-top —
a band box dragged up or left — where the inner outline then lands one pixel *outside* the
outer one. Ours had exactly that bug until the G13o landing review; the G13n A/B that passed
"0 differing pixels" had only ever dragged down-right. [FROM REVIEW, then confirmed against the
disassembly and re-measured: an up-left drag now diffs to 0 px against the engine's own.]

### `KeyboardHotkeySampler 0x4C1B80` — why polling it twice is safe

`ui-markers.md` relies on this and it is worth having in the map. The function is a jump table
(`0x4C1C48`, index bytes at `0x4C1C6C`); the SHIFT entry `0xF9` lands at `0x4C1BB5`, which is
`push 0x10; call ds:0x4FC350` (`GetAsyncKeyState`) then **`and al,0xfe`** — masking off bit 0,
the "pressed since last call" bit — before `neg ax; sbb eax,eax; neg eax` normalises to 0/1.
All nine stubs in the `0x4C1BA1..0x4C1D56` block do the same mask, so nothing in the engine
reads the consumable bit and an extra poll of our own cannot steal an edge.

## The order-marker chain — mapped by us

[MEASURED 2026-09-04, this project — `objdump -d -M intel` of the pristine Steam build
(md5 `8e74a1dffa1f5988624c52048f5b20cd`), every function below read instruction by
instruction for this landing, plus a live 16 650-record trace against the engine's own
drawer calls. This is the block `tagpu_order.c` now re-draws instead of capturing;
`ui-markers.md` §3 is the same chain written from the marker's point of view.]

The shift-held overlay is one driver, one list walker and five leaf drawers, reached from
a single call site inside `DrawGameScreen`.

```
0x4699EB  call 0x46A530                             selection rect, ground sweep
0x469B8A  call 0x46A530                             selection rect, air sweep
0x469BD7  hook 8                                    (was capture window A; G13p
                                                     retired it — the hook now only
                                                     brackets our order arena's block
                                                     and latches the text font)
0x469BE1  KeyboardHotkeySampler(0xF9)  -> SHIFT?    (0x469BE8 je 0x469C01)
0x469BFC  call 0x48CC30(ctx, main+0x142F3)          <- the driver, SHIFT-gated
0x469C03  je 0x469D38                               (drawUnits == 0 exits past hook 9)
0x469CB9  call 0x46A430                             health bars, AFTER the markers
0x469CF9  call 0x4C14F0                             group digits (G13p: SKIPPED)
0x469D2C  hook 9
0x469EC5  call 0x4BF8C0                             build cursor / band box, outer
0x469F1E  call 0x4BF8C0                             ...and the inner rect, after fog
```

### `0x48CC30` — the driver, and the three selection rules

`stdcall(OFFSCREEN* ctx, void* viewStruct)`, `ret 8` @ `0x48CD70`, sole caller `0x469BFC`.
Five register pushes, so `ctx` is `[esp+0x18]` and `viewStruct` `[esp+0x1c]` inside.

It resolves three units before the loop, and each is `0` when its id is `0`:

| What | Where | Read at |
| --- | --- | --- |
| `CameraToUnit` | `viewStruct+0` (`main+0x142F3`) | `0x48CCB0` |
| tracked unit | `units(main+0x14357) + main[0x37E9C]·0x118` | `0x48CC58..0x48CC81` |
| hovered unit | `units + main[0x2CBA]·0x118` | `0x48CC84..0x48CCA9` |

`bl` is set at `0x48CCEE` iff ANY of those three has a non-null `UnitDef+0x156`
(`CANBUILD_ptr`) — the "hover a constructor and see everyone's claimed build sites" rule.
Note the three tests at `0x48CCB6/0x48CCCA/0x48CCDE` dereference `unit+0x92` without a null
check; ours adds one.

The walk is over the WATCHED player's own unit range —
`PlayerStruct = main+0x1B63 + main[0x2A42]·0x14B` (`0x48CC3B..0x48CC51` builds the stride
without a multiply, as `cl + ((cl·33)·5)·2` = `cl·331`), first `+0x67`, last `+0x6B`, step
`0x118`, bounds compared **unsigned**
(`ja` @ `0x48CCFC`, `jbe` @ `0x48CD69`) and the end pointer **re-read every iteration**.

```
st = unit[0x110]
if (!(st & 0x10000000)) continue                    alive        0x48CD08
if (st & 0x4000)        continue                    excluded     0x48CD10 (test ch,0x40)
if (unit == CameraToUnit)                mask=0x1F flag=1        0x48CD15
else if (unit[0xA8] == main[0x37E9C])    mask=0x1F flag=1        0x48CD20
else if (unit[0xA8] == main[0x2CBA])     mask=0x1F flag=1        0x48CD29
else if (st & 0x10)                      mask=0x1F flag=0        0x48CD32 (shr ecx,4; test cl,1)
else if (bl)                             mask=0x01 flag=1        0x48CD3E
else                                     continue
0x439B30(unit, mask, ctx, viewStruct, flag)                      0x48CD51
```

The two id comparisons are made **without** first checking the id is non-zero, so a unit
whose `UnitInGameIndex` is 0 matches a tracked/hovered id of 0. Reproduced as-is.

### `0x439B30` — the walker, its dispatch table, and where `pos` is restored

`ret 0x14`, args `(unit, mask, ctx, view, flag)`. `sub esp,0xC` then four pushes: the three
dwords at `[esp+0x10..0x18]` are `pos`, seeded from the unit's own 16.16 triple at `+0x6A`
(`0x439B40..0x439B62`), and **the `unit` argument slot `[esp+0x20]` is reused as the
range-circle guard byte** (cleared at `0x439B47`, set at `0x439CC3`).

Per node — head `unit+0x5C`, next `node+0x4A` (`0x439CC8`) — the order type byte `node+0x4`
indexes the descriptor array behind `*(u32*)0x512344`:

```
eax = node[0x4];  eax *= 5;  edx = base + eax*4;  mask = [eax + edx + 0xC]
                                     i.e.  base + type*0x19 + 0xC
```
recomputed from scratch ahead of every one of the five tests (`0x439B6C`, `0x439BB1`,
`0x439BF7`, `0x439C3C`, `0x439C82`), ANDed with the caller's mask each time.

| bit | drawer | call site |
| --- | --- | --- |
| 0 | `0x438C00` build-site footprint rect | `0x439BAC` |
| 1 | `0x4394E0` route dots (delegates to bit 3 first, unconditionally) | `0x439BF2` |
| 2 | `0x4399F0` circle around the order target | `0x439C37` |
| 3 | `0x439740` animated target sprite | `0x439C7D` |
| 4 | `0x4390A0` per-unit range circles, behind the guard byte | `0x439CBE` |

**`pos` IS RESTORED TO THE NODE'S ENTRY VALUE BEFORE BITS 0..3 AND NOT BEFORE BIT 4.** The
walker keeps a register copy of the node-entry position in `edi`/`ebp`/`ebx` (reloaded from
`pos` at `0x439CCB..0x439CD3`, i.e. at the END of each node) and writes it back into `pos`
with the three stores `mov [esp+0x24],edi` / `[esp+0x28],ebp` / `[esp+0x2c],ebx` immediately
before the calls at `0x439BAC`, `0x439BF2`, `0x439C37` and `0x439C7D`. There is **no such
triple ahead of `0x439CBE`**. So every one of bits 0..3 starts from the same point, the chain
advances by whichever of them ran LAST, and the range-circle drawer sees whatever they left.
[This is the fact the trace was built to check, and it holds: 16 650 records, zero
disagreements — `git log` "Three things the live bring-up found".]

### `0x512344` — the order-descriptor array

`0x512344` begin, `0x512348` end, `0x51234C` capacity: a vector of **25-byte** records, all
three zeroed by the constructor at `0x438450` and freed at `0x438480` through `0x4B4F20`. A
lookup by name walks it with `strcmp 0x4F8A70` against the pointer at `record+0x15`
(`0x4387A9`, `0x4387E4`), which is what fixes both the stride and that last field.

| Offset | What | Read at |
| --- | --- | --- |
| `+0x00..0x0B` | unread by any of the marker paths | — |
| `+0x08` | the target-sprite drawer's own address, **never read in this build** | — |
| `+0x0C` | u32 marker-capability mask (bits per the table above) | `0x439B7D` etc. |
| `+0x10` | u8 `cursor_ary` index; 0 = this order type draws no sprite | `0x4397F3`, `0x439965` |
| `+0x15` | `char*` name, for the console lookup | `0x4387A9` |

The engine indexes this with the raw type byte and no bound at all. With a live end pointer
in `0x512348` the bound costs two loads, so ours takes it.

### `0x438C00` — the build-site footprint rect

`ret 0x14`. Nothing at all unless `node+0x36` (the build target's unit type id, u16) is
non-zero — the early exit at `0x438C0E` skips the `pos` chaining as well as the draw.
`def = UnitDefs(main+0x1439B) + type·0x249` (`0x438C1D..0x438C38`, the multiply written as
`(type·65)·9`).

```
P0 = (t.x + def[0x15E],  t.y + def[0x162],  t.z + def[0x166])     t = node+0x22 (16.16)
P1 = (t.x + def[0x16A],   --                t.z + def[0x172])     def[0x16E] is never read
alt = (s16)(P0.y >> 16)
x0  = (s16)(P0.x>>16) - view[0x2C] + 0x80        x1 likewise from P1.x
z0  = (s16)(P0.z>>16) - (alt>>1) - view[0x30] + 0x20      z1 likewise from P1.z
```

Then the ten-tick animation (`0x438CB7..0x438D3D`), and **it sweeps the four edges INWARD,
it does not grow an inner rect** — `ui-markers.md` said "grows" and that was wrong:

```
t   = gameTime(main+0x38A47) - node[0x46]
t   = ((unsigned)t >= 10) ? 10 : t        <- UNSIGNED: a negative age reads as FINISHED
dxg = (x1-x0)*t/10        dzg = (z1-z0)*t/10        both /10 by the 0x66666667 magic
xg0 = x0+dxg   xg1 = x1-dxg   zg0 = z0+dzg   zg1 = z1-dzg
```

At `t=0` the animated positions sit on the rect's own edges; at `t=10` they have swapped to
the opposite ones exactly — `xg0 = x1` and `xg1 = x0` — so the colour-B lines land ON those
edges and the colour-A pair, drawn one pixel outside each animated position, lands one pixel
inside them. Colour pair from the ISSUING unit `node+0xE`'s
`stateMask & 0x10` (`0x438D41..0x438D85`): selected → `main[0xDCE]`(gui 3) + `main[0xDD5]`
(gui 0xA), not → `main[0xDCC]`(gui 1) + `main[0xDD4]`(gui 9).

Eight `DrawLine 0x4BE950` calls, four in colour A one pixel outside the animated positions
and spanning one pixel past the corners, four in colour B exactly on them
(`0x438DAA`, `0x438DCE`, `0x438DF5`, `0x438E18`, `0x438E34`, `0x438E47`, `0x438E5E`,
`0x438E6D`). Finally `pos ← node+0x22..0x2A` (`0x438E74..0x438E8B`).

**`DrawLine 0x4BE950` is `stdcall(ctx, x0, y0, x1, y1, colour)`** — fixed by those eight
call sites, where the first and third pushed values are the two x's.

### `0x4394E0` — the route dots

`ret 0x14`. Saves `pos` into locals, calls `0x439740` with the SAME `pos` pointer
(`0x439516`) so the sprite's resolved position becomes the segment's far end, then returns
at once unless `flag == 1` (`0x43951D`) — a merely *selected* unit gets no route line.

```
len   = (int)sqrt(dx² + dy² + dz²)      dx,dy,dz = pos - saved, 16.16   0x43956A..0x43957C
if (len < 0x10000) return                                               0x439587
age   = gameTime - node[0x46]
phase = ((age % 30) * 0x300000) / 30            48.0 world units per 30 ticks
seq   = *(main+0x148D3)                          the `pathicon` GAF sequence
period= (u16)seq[0x2C] ? that : 1                = frametab[0]'s duration dword
frame = (age / period) % (u16)seq[0]
for (cursor = phase; cursor < len; cursor += 0x300000) {
    p = saved + delta * ((int64)cursor << 16) / len >> 16      __alldiv/__allmul/__allshr
    CopyGafToContext 0x4B7F90(ctx, seq[0x28 + frame*8], px, pz)
    frame = (frame + 1) % nframes
}
```

`seq[0x2C]` is not a field of its own: `TAGPU_SQ_TAB` is `0x28` with stride 8, so it is the
u32 half of frame 0's table entry.

### `0x4399F0` — the circle around the order target

`ret 0x14`. Centre and radius: with `node+0x16` non-zero, the target unit's own 16.16 triple
and `(s16)def[0x178]`; with a ground target, `node+0x22..0x2A` and a flat `0x20`
(`0x4399F7..0x439A4C`). The y radius is `(int)(R * 0.89)` — the double at **`0x4FD2C0`**.
Sixteen chords, angle `0x1000` to `0x10000` step `0x1000`, vertices
`(cx + TurnZLookup 0x4B7123(angle,R), cz + TurnXLookup 0x4B70EF(angle,R2))`, colour
`main[0xDD7]` (gui 0xC). Both lookups are **cdecl** (`add esp,8` after each). `pos ← centre`.

### `0x439740` — the target sprite, and the one sim write in the block

`ret 0x14`. Resolves the position first:

```
tgt = node[0x16]
if (!tgt)                       p = node[0x22..0x2A]              ground target
else if (UnitInPlayerLOS 0x465AC0(node[0xE][0x96], tgt))          stdcall(player, unit), ret 8
                                p = tgt[0x6A..0x72]
                                node[0x32] = (s16)(p.x>>16)       <- WRITES THE CACHE
                                node[0x34] = (s16)(p.z>>16)
                                node[0x42] |= 0x200000
else if (node[0x42] & 0x200000) p = ((s16)node[0x32]<<16, tgt[0x6E], (s16)node[0x34]<<16)
else                            the live branch above
```

That cache write is the only thing in the whole marker block that touches sim-side state,
and it is what stops a waypoint marker following a target the player can no longer see. A
port that drops it leaks the target's live position; ours reproduces it on the game thread
at the same instant (`tagpu_order.c`, `resolve_sprite`).

Then, only if the descriptor's `+0x10` cursor index is non-zero (tested at `0x4397F7`, with
`0x4397F9` the branch past the early exit that otherwise chains `pos` and returns — so a zero
index skips the ShowRanges limb below as well as the sprite), and reaching its own sequence
lookup at `0x439952`, frame `(gameTime / (2·period)) % nframes` of
`cursor_ary[idx]` = `*(main+0x1487F + idx*4)` is alpha-blitted through
`AlphaCompsteBuf2OFFScreen 0x4B8500` (`0x4399BB`). `pos ← p`.

`0x439811..0x439948` is a `ShowRanges`-only limb, and it draws CIRCLES as well as labels: for
descriptor cursor index 1 or 2 it emits, through `0x438EA0` and **at the resolved target
position**, each live weapon's AoE (`w+0xD6`) and `attackrunlength` (`w+0xE0`) plus
`def[0x216]`, in the same `gameTime & 1` flash colour. Its weapon-slot flags are the regular
`0x1F + i·0x1C` (`0x439879`), so this drawer does **not** carry `0x4390A0`'s third-slot quirk
— the two disagree about the same question in the same build.

Its labels are **formatted at draw time**, through the engine's own `sprintf 0x4E42B0`
(called at `0x43989B` and `0x4398DE`):
`"weapon %d - area of effect"` (`0x5051C4`, at label slot 0) and `"weapon %d - coverage"`
(`0x5051AC`, slot 1), both taking the weapon INDEX 0..2, plus the literal `"attack length"`
(`0x50519C`, slot 2) for `def[0x216]`. [BINARY-VERIFIED 2026-09-05]

### `0x4390A0` — the per-unit range circles

`ret 0x14`. `unit = node+0xE`, `def = unit[0x92]`. With `ShowRanges` (`main+0x391BF`) clear:

```
if (def[0x208] && (unit[0x10E] & 4))                                 cloaked
    DrawRangeCircle(ctx, view, unit+0x6A, (s16)def[0x208], main[0xDDA], 0, 0)
if (!(def[0x241] & 0x10000000)) return                               not kamikaze
w = def[0x220]; if (!w) return                                       ExplodeAs
aoe = (u16)w[0xD6] >> 1
r   = ((gameTime % 60) * aoe * 2) / 60      unsigned throughout      0x43913E..0x43915E
r   = max(8, min(r, aoe))
DrawRangeCircle(..., r,                       main[0xDD7], 0, 0)
DrawRangeCircle(..., unit[0x0] ? (u16)def[0x218] : (s16)def[0x202], main[0xDD7], 0, 0)
```

With `ShowRanges` set it draws a labelled set of NINE instead, and the first of them is
`def[0x208]` — the cloak radius — at `0x43921A`, gated **only on the value being non-zero and
not on the unit's cloak flag**, which is the normal branch's rule and not this one. Then
`def[0x202]` sight, `+0x204` radar,
`+0x206` sonar, `+0x20A` radar jam, `+0x20C` sonar jam, `+0x212` builddistance, `+0x214`
maneuver, `+0x218` kamikazedistance, all in `main[0xDD9]` (gui 0xE) with label strings at
`0x505190/88/80/78/6C/60/50/44` and `0x503A0C` — `"mincloak"`, `"sight"`, `"radar"`,
`"sonar"`, `"radarjam"`, `"sonarjam"`, `"build distance"`, `"maneuver"`,
`"kamikazedistance"`; then the three weapon ranges (`unit+0x10`,
`+0x2C`, `+0x48`, each `+0xDC`) flashing `main[0xDCF]`/`main[0xDD7]` on `gameTime & 1`, the
first gated at `0x439443`, labelled `"weapon1 range"` `0x505134`, `"weapon2 range"`
`0x505124`, `"weapon3 range"` `0x505114`.

**The `labelSlot` each one passes** [BINARY-VERIFIED 2026-09-05]: for the nine it is `esi`,
a running count of the circles actually drawn — 0 for the cloak radius (a literal, with
`mov esi,1` after it at `0x439236`), then `mov eax,esi; inc esi` for each of the next seven,
and a bare `push esi` for kamikazedistance because it is the last. For the three weapon
ranges it is a **literal 0, 1, 2** (`0x439457`, `0x439485`, `0x4394B0`), so a unit with all
twelve draws two labels at slot 0, two at 1 and two at 2.

**Those nine radii are read with MIXED sign, and the mix is not tidy** — worth having written
down, because a blanket cast either way is a guess: `movsx` for cloak `0x439229`, sight
`0x439267`, radar `0x4392A1`, sonar `0x4392DB`, radar-jam `0x439315` and sonar-jam `0x43934F`
(each after a `mov dx,WORD PTR [eax+…]` at `0x43924A`, `0x439284`, `0x4392BE`, `0x4392F8`,
`0x439332`), and `and 0xffff` for builddistance `0x43937A`, maneuver `0x4393B7` and
kamikazedistance `0x439404`. Inert for any value below 32768, which every stock one is.

**An engine quirk worth knowing before it looks like a bug in a port:** weapon 1 is gated on
`unit[0x1F] & 2` and weapon 2 on `unit[0x3B] & 2` (= `0x1F + 0x1C`), but weapon 3 is gated on
`unit[0x1F] & 2` **again** at `0x43949D`, where `0x1F + 0x38 = 0x57` was meant. Reproduced
rather than corrected — a "fix" would draw a circle the engine never draws.

### `DrawRangeCircle 0x438EA0` — the terrain-following circle

`ret 0x1C`: `(ctx, view, POS16_16* centre, radius, colour, char* label, labelSlot)`. A zero
radius returns at once (`cmp ebx,ebp` at `0x438EAC`, `je 0x43908F` at `0x438EAF`). The
segment count is `(int)(radius · 2π · 0.125)` — the doubles at **`0x4FD2B0`** (2π) and
**`0x4FD2B8`** (0.125) — i.e. one segment per 8 world units of circumference, and the loop
runs `i = 0..N` inclusive, stepping `0x10000/N` in angle units per segment.

**It is a ROUND circle — there is no isometric squash here** [BINARY-VERIFIED 2026-09-05].
`ebx` is reloaded with `radius<<16` at the top of every iteration (`0x438F08` from
`[esp+0x18]`) and handed to **both** `TurnXLookup` and `TurnZLookup`. The 0.89 at `0x4FD2C0`
belongs to the TARGET circle `0x4399F0`, which multiplies **only** its y radius by it
(`[esp+0x18]` there, against the unsquashed `[esp+0x34]` on x). G13o applied the squash to
both drawers and drew every range circle 11 % flat until G13p; the projection maps world z to
screen y 1:1, so a round circle in world space is a round circle on screen.

**And it can divide by zero.** `mov eax,0x10000` at `0x438EE4`, `cdq`, then **`idiv ecx` at
`0x438EEE`** with `ecx` = N, guarded only by `jl` against a *negative* N (`0x438EDE`). A radius
of 1 gives `(int)(1 · 0.7854) == 0` and faults inside TA. Nothing in stock content is that
small.

**`TurnXLookup 0x4B70EF` is a SINE and `TurnZLookup 0x4B7123` a COSINE**, off one shared
table at **`0x509F00`**: 512 `s16` entries, `8192 = 1.0` (`shrd …,0xD` after a `+0x1000`
round), indexed `((angle + 0x20) >> 6) & 0x3FE`, and TurnZ adds a quarter turn (`+0x4020`)
before the same shift. Both are **cdecl** — the callers `add esp,8`. So the engine's
parameterisation is `p.x = centre.x + r·sin(a)`, `p.z = centre.z + r·cos(a)`, which is the
mirror of the usual cos/sin pair: on a round circle it is the same circle entered a quarter
turn along, and it matters only when you want a point on it at the engine's own angle.

Each endpoint is `centre ± TurnX/TurnZLookup(angle, radius<<16)` and then, and this is what
makes these circles hug the ground, its altitude is raised:

```
p.y_hi = max((s16)centre[+6], GetPosHeight 0x485070(&p))            0x438F47, 0x438F5C
```

before the standard `- alt/2 - eyeY + 0x20` projection. `GetPosHeight` is
`stdcall(POS16_16*)`, `ret 4`, and reads only the two high words `[p+2]` and `[p+0xA]`; it is
a pure bilinear read of the height grid at `main+0x14287` with dims `main+0x14233` /
`main+0x14237`, no writes and no globals of its own, which is why our render thread may call
it directly.

**The label's anchor, exactly** [BINARY-VERIFIED 2026-09-05]. `labelSlot` is multiplied by
three once, before the loop (`lea eax,[eax+eax*2]` at `0x438EFF`), and the loop remembers
`(x1, y1)` — the **second** endpoint, at angle `(i+1)·step` — of the iteration whose index
equals it (`0x43902E`). So the anchor is the point at angle `(labelSlot·3 + 1)·(0x10000/N)`.
If nothing matched (`labelSlot·3 > N`) both remembered values are still 0 and `0x43906D`
falls back to the LAST endpoint computed, i.e. `(N+1)·step`. The string is then
`DrawTextCustomFont 0x4C14F0(ctx, str, x, y + 4, -1)` — `add edx,0x4` at `0x43907D` — in
whatever colour `SetTextColors` last set, **not** the circle's.

### The in-game bitmap font, and how to rasterise TA's glyphs into a buffer of your own

[BINARY-VERIFIED 2026-09-05, this project. Read while scoping the text half of the marker
port and then USED by it — `tagpu_text.c` calls `0x4CCF60` directly. An earlier revision of
this section, written from the measure loop alone, called `font+0x00` a "baseline offset";
it is the glyph ROW COUNT, and `font+0x02` is the offset it was confused with.]

`DrawTextCustomFont 0x4C14F0` is `stdcall(OFFSCREEN* ctx, const char* str, int x, int y,
int maxWidth)`, `ret 0x14`. Its font object is `[globals+0x204]` (`0x4B6220` is just
`mov eax,ds:0x51FBD0; ret`), and the layout both its measure loop at `0x4C1527` and the
blitter read is:

```
font+0x00   u8    glyph height in ROWS       (also y+this = the measured box's bottom, 0x4C1659)
font+0x02   s8    a row offset the BLITTER SUBTRACTS from the y it is given
font+0x03   u8    first character code
font+0x04   u16[] per-character offset, indexed by (char - first); 0 = glyph absent
font+off    u8    that glyph's width, in pixels AND in bits
font+off+1  ...   the glyph: rows x width bits, MSB first, packed ACROSS row boundaries
```

Neither the measure nor the blit bounds the index against the table's length — a character
past its end reads whatever follows — and both skip a code below `first` and a zero offset
**without advancing the cursor**. Those two are the ONLY characters the blit skips: `sub
ebx,[ebp-0x8]` at **`0x4CCFAA`** with `jb 0x4CCF91` at `0x4CCFAD`, and `or ebx,ebx` at
**`0x4CCFB9`** with `je 0x4CCF91` at `0x4CCFBB`. There is no upper bound anywhere, so anything
calling `0x4CCF60` has to make its own measure agree with that character for character or the
blit runs past the width the caller reserved.

**`ctx` may be NULL**, in which case `0x4C14F0` locks the screen surface itself
(`0x4C5E70(&localOFFSCREEN)`, `0x4C5FA0` to release) and draws into that.

**It does not clip the string — it REJECTS it.** `0x4C6AE0` is
`OFFSCREEN::GetClipRect(RECT* out)`, thiscall, `ret 4`: four dwords copied from `this+0x1C`.
`0x4B6750(RECT* inner, RECT* outer)` is a **containment** test — eight compares, `0` unless
every edge of `inner` lies inside `outer` — called at `0x4C1697` (ctx == NULL arm) and
`0x4C1709`, with `0x4C169E`/`0x4C1710` skipping the blit entirely when it fails. So a string whose measured box `{x, y, x+width, y+font[0]}` is not wholly
inside the context's clip rect is not drawn short: it is not drawn at all. That is why the
group digit and the `ShowRanges` labels vanished in the outer ring at zoom < 1, and why
G13p ports them rather than widening anything.

#### `0x4CCF60` — the blitter, which takes its destination directly

**cdecl, nine arguments** (`add esp,0x24` at `0x4C16D9`):

```
0x4CCF60(u8* base, int pitch, void* font, const char* str, int x, int y,
         int fg, int bg, int transparent)
```

and `0x4C14F0` fills the first two from **its OFFSCREEN's own pixel base and pitch** —
`push [edi+0xC]` / `push [edi+0x8]` at `0x4C173E`/`0x4C1738` on the ctx path, and the same
two fields of the locally locked surface at `0x4C16A0`. Arguments 7, 8 and 9 are
`[globals+0x208]`, `[globals+0x20C]`, `[globals+0x210]`. **No OFFSCREEN reaches it, no clip
rect, not even a width or a height** — it writes exactly `sum(widths) × font[0]` pixels at
`base + (y − (s8)font[0x02]) · pitch + x` (`sub eax,ebx` at **`0x4CCF87`**, then an
**unsigned** `mul` by the pitch at `0x4CCF89`) and it is the caller's business to have measured
that. Which is the whole reason a port needs no font RE at all: hand it a buffer of yours
and TA rasterises its own glyphs into it, at any size, with none of the `0x4CC650` surface
bound that clips the line drawers.

Per pixel it is `colour = bit ? fg : bg; if (colour != transparent) *dst = colour` — a
transparent equal to `bg` makes the background bits no-ops, so `(255, 0, 0)` turns the call
into a **1-bit coverage mask** rather than a coloured sprite. The bit counter (`dl`) is reset
per GLYPH at `0x4CCFC8` and not per row, so only each glyph starts on a byte boundary.

#### The text globals, and who sets them

| VA | What |
| --- | --- |
| `0x4C1420` | `SetFont(font)` → `[globals+0x204]`, ignoring NULL; `ret 4` |
| `0x4C13A0` | `SetTextColors(fg, bg)` → `[globals+0x208]` / `[globals+0x20C]`, each **skipped when the argument is −1**; `ret 8` |
| `0x4C13D0` | `SetTextTransparentColor(c)` → `[globals+0x210]`; `ret 4` |
| `0x4C13F0` / `0x4C1400` / `0x4C1410` | the three getters |

All three are process-global and the engine re-points them many times a frame, so anything
reading them off another thread gets whatever the game thread last drew with. Inside
`DrawGameScreen` the last pair before the marker block is **`0x4696CD`/`0x4696E7`**:
`SetFont` from a table at `main+0x3816B`, then `SetTextColors(gui[0xF], GetTextTransparentColor())`
— i.e. the in-game text is `main[0xDDA]` on a background equal to the transparent index, so
its background bits store nothing. Between that pair and the group digit at `0x469CF9`
nothing calls either function: a full-image scan puts every `0x4C1420` and `0x4C13A0` call
site outside `0x4696E7..0x469CF9`, the order driver, the walker, the five leaf drawers and
`DrawHealthBars` included. One latch at hook 8 therefore holds for the whole block.

### `0x469CD1..0x469CF9` — the group digit

[BINARY-VERIFIED 2026-09-05. The block `tagpu_mark.c` now draws.]

```
0x469C4A  if (!(main[0x37F06] & 1) && *(u32*)(unit+0xAC) == 0) continue;
0x469C63  sx = (s16)unit[0x6C] - main[0x1431F] + 0x80
          sy = (s16)unit[0x74] - main[0x14323] - ((s16)unit[0x70] >> 1) + 0x20
0x469C6B  buf[1] = 0                                   the string's terminator
0x469C97  if (!(main[0x37F06] & 1)) continue;           "damagebars" gates the WHOLE unit
0x469CB9  if (unit[0x96]->id == p) DrawHealthBars(ctx, unit, sx, sy + 0x0A)
0x469CD1  if (unit[0x96]->id == p && *(u32*)(unit+0xAC))
0x469CF9      DrawTextCustomFont(ctx, {'0' + (u8)unit[0xAC], 0}, sx, sy + 0x0E, -1)
```

**`p` is `main+0x2A43`, and it is NOT the byte the order driver uses**
[BINARY-VERIFIED 2026-09-05]. `DrawGameScreen` loads it into a local at
`0x46967D` (`mov al,[edx+0x2a43]`) and stores it at `0x469689`; both the bar
(`0x469CA6`) and the digit (`0x469CC9`) compare `[[unit+0x96]+0x146]` against
that `[esp+0x70]`. The order-marker driver `0x48CC30`, in the same block, picks
its player range from **`main+0x2A42`** instead (`0x48CC3B`). The pair is written
independently — `0x416B25` and `0x416B38`, from two separate calls in one loader
function — so the two can hold different values, and code that reproduces either
loop has to use the byte that loop uses.

**Which of the pair is "watched" and which "local" is NOT established here, and
this note's own pages disagree** [INFERRED, unresolved]: `ui-markers.md`'s
appendix calls `+0x2A42` watched and `+0x2A43` local, while `effects.md`,
`features.md` and `line-of-sight.md` all call `+0x2A43` the local player and the
"Mapped internal data structures" table below calls `+0x2A42` the local player
index. Nothing in this landing needed the names — only the addresses — so the
question is left open rather than guessed at.

Two further things are not obvious and both matter to a port. **The squad tag is
tested as a DWORD** — `mov ecx,[edi+0xac]; test ecx,ecx` at both `0x469C55` and `0x469CD1` — and only
then used as a byte, so a unit whose `0xAD..0xAF` are non-zero draws a `'0'`. And **the digit
sits four rows below the bar**, `sy + 0x0E` against the bar's `sy + 0x0A`. There is no health
test on the digit: `DrawHealthBars` returns early on a dead unit, but the digit is drawn from
the caller and does not care.

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
| `0x4592C8` `test [esp+0x10],0x40000000` (after the structure test) | `0x4594D0` `shr edx,0x1E; test dl,1` (**before** the structure test) | `digger`. Path A sends it to the COMPLETED branch, TShadow and `canhover`/`floater` tests included. Path B takes an **inline** branch `0x4594D8..0x45951D` — `0x45A470` silhouette, `0x4BA1B0(scratch, 0x7D)` ground clip, then `jmp 0x4595E9` to the shared blit — and applies neither test. So a digger never reaches the cached branch in either path |
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

**The two rasterisers `owndraw` already detours**, for completeness of this map: `0x459830`
(opaque, `thiscall`, `ret 0x10`; called from the builder `0x45878B` and from the blit's
build-state path `0x459641`) and `0x459C70` (nanoframe, called from the builder `0x458765`).
Both open `mov eax,imm32` (5 bytes) before `call __chkstk`, which is the detour boundary;
evidence and the classify-then-`ret 0x10` stub: `own-the-draw.md`, `tagpu_owndraw.c`.

**`0x459228..0x45927A` — where the body and the shadow are placed, register by register.**
[BINARY-VERIFIED 2026-09-04, read for the aircraft work.] `[ebp+0xc]` is the unit; `+0x6A`,
`+0x6E`, `+0x72` are X, altitude and depth as 16.16.

```
459233  mov ebx,[eax+0x6a]  / 459239 sub ebx,[esp+0x3c]   ; X - eyeX
45923d  push ecx            ; ecx = &unit.position (eax+0x6a)
459242  mov ebx,[eax+0x6e]  ; altitude          -> saved
459245  mov eax,[eax+0x72]  / 45924c sub eax,edx          ; Z - eyeY
459252  call 0x485070       ; GetPosHeight(&pos) -> eax = the GROUND under the unit
459257  movsx edx,[esp+0x42]  ; altitude, high word = whole world units
45925c  movsx ecx,[esp+0x46]  ; (Z - eyeY) high word
459261  mov ebx,edx           ; ebx := altitude       <- these two copies are what
459263  mov [esp+0x14],edx    ;   the altitude the waterline code re-reads at 0x45959F
459267  sar ebx,1             ; ebx := altitude / 2
459269  mov edx,ecx           ; edx := (Z - eyeY)     <- ...make the next lines read right
45926b  sar eax,1             ; eax := GROUND / 2   (0x485070's return)
45926d  sub edx,ebx  / 459274 add edx,0x20    ; bodyY   = (Z-eyeY) - altitude/2 + 0x20
45926f  sub ecx,eax  / 459277 add ecx,0x20    ; shadowY = (Z-eyeY) - ground/2   + 0x20
```

So the two Y values differ **only** in which height is halved, and the shadow's x is the body's
`sx + 0x80` plus 5 (`add edx,0x85` in the branch table above). For anything on the ground
`GetPosHeight` returns the unit's own altitude and the two coincide; for an aircraft the shadow
stays on the ground and trails the body down the screen by exactly `(altitude − ground) / 2`.
Measured in play the same day — see [shadows & cloak](shadows-cloak.html) §"Aircraft, measured".

**`[esp+0x42]` is the altitude.** `0x459257 movsx edx,word [esp+0x42]` is stored at
`0x459263 mov [esp+0x14],edx`, and `[esp+0x14]` is what the waterline code subtracts from sea
level at `0x45959F` — the same word, sign-extended (the first draft of this section called them
different words; the G13k review corrected it). The model-0 skip therefore compares the unit's
altitude with sea level, which is what the native pass's `fz` gate does.

**Negative results.** The body punch-out `0x4B9D70(body, scratch, 5, 0)` inside `0x45A790` was read, not
replicated. `0x459200` itself was not disassembled past `0x459900`.

**A unit under construction casts no shadow until it is nearly finished, and this tree does not
say why.** [MEASURED 2026-09-03] One ARM solar, same ground, same camera, held at 25 / 50 / 75 /
85 / 89 / 95 / 99 / 100 % built by the scenario applier and captured off the stock renderer (no
passes armed). Taking the pixels the *completed* unit darkens by half as the shadow lobe, and its
own 25 % frame as the bare-terrain reference, the mean luminance over that lobe reads **1.00** at
25 %, **0.82** at 75 %, **0.84** at 85 %, **0.87** at 89 %, **0.70** at 95 %, **0.71** at 99 % and
**0.48** complete — no shadow for most of a build, something partial in the last few per cent.
**Caveat on the fixture:** the applier creates the unit complete and then writes `+0x104`, so its
composite and cached shadow have a history a lathed unit's does not; the recolour classifies this
model identically at `p` 28 and 12, so the 89→95 % step is not explained by the staging and may
be an artefact of that history. Nothing in the branch table above tests
`Nanoframe`: a building is a structure whether finished or not (see the state-bit measurement
below), so it reaches `0x45955B`/`0x4592FE` and blits `Object3do+0x14` either way. The
resolution is therefore in what that cached sprite CONTAINS while the unit is a nanoframe —
whose composite the dispatch `0x458810` throws away on every progress pulse
([build-state](build-state.html) §1) — and that was not chased. Recorded as behaviour: our own
pass suppresses the shadow on `Nanoframe != 0`, which matches every row to 89 % and is
conservative for the last two.

**`unit+0x110 & 0x20000000` is the STRUCTURE bit.** [MEASURED 2026-09-03] Read live in one game
from `*(main+0x14357) + idx*0x118 + 0x110`: complete mobile ARMCOM `0x91600371` (clear),
complete building ARMSOLAR `0x30282321` (set), ARMLAB **under construction** `0x30E42321` (set).
So the table above is right to call it the structure bit — as the factory-built check of
2026-09-02 in [shadows & cloak](shadows-cloak.html) already found from the other side (a
Peewee out of an ARMLAB reads it CLEAR) — and [build-state](build-state.html)'s
"under-construction/nanoframe state" reading was wrong, corrected there. The consequences are
larger than a name: `0x45873C` selects the Gouraud rasteriser `0x459C70` for **structures**, not
for nanoframes, and the only state that means *under construction* is `Nanoframe != 0` at
`+0x104`. The spawn site `0x485AFE..0x485B03`, which that page cited as where the bit is set,
computes `(UnitDef+0x241 & 0x200) << 0x15` = bit **`0x40000000`** and writes only that.

## The order module — where an order's position lives, and in what units — mapped by us

[MEASURED 2026-09-04, this project — `objdump` of the pristine Steam build, plus a live A/B on
Two Continents. Established while fixing the scenario applier, whose orders all walked to the
map origin.]

**`ORDERS_NewMainOrder2Unit 0x43AFC0`** — stdcall, `ret 0x1C`, seven args
`(actionIndex, shift, unit, target, pos, p1, p2)`; `pos` is a pointer to **three 16.16
dwords**.

**Its first branch is on `shift`, and with `shift = 0` almost none of the function runs**
[BINARY-VERIFIED, corrected by the landing review — the first draft of this section described
the scan as unconditional]:

```
43afc0  mov  eax,[esp+0x8]      ; arg2 = shift
43afd4  test eax,eax
43afda  je   0x43b08a           ; shift == 0 -> straight to the allocator
43afe0  mov  esi,[edi+0x5c]     ; else walk the order list, head ALWAYS +0x5C
```

`0x43B08A` re-pushes the arguments and calls the allocator **`0x43ADC0`**, which takes `0x56`
bytes (`push 0x56; call 0x4B4F10` at `0x43ADC4`) and constructs the order in place with
**`0x43A0C0`** (`thiscall`, `ecx` = the new order). The scenario applier passes `shift = 0`, so
**that is the whole of its path** and everything below about the scan is context, not
description of what we do.

**What the `shift != 0` scan does when it matches is CANCEL, not merge**
[BINARY-VERIFIED `0x43B034..0x43B087`]: `0x43B03D` picks `unit+0x60` over `+0x5C` as the list
head to unlink from when the matched order's flag word `+0x42` has `0x40000` (this is the
*unlink* head — the walk itself started unconditionally at `+0x5C`), `0x43B05B..0x43B05E`
unlinks it, `0x43B075 call 0x43A1F0` destructs it, `0x43B07B call 0x4B4F20` frees it, and
`0x43B087 ret 0x1C` returns **without creating anything**. That is TA's shift-click-a-waypoint-
to-remove-it behaviour, not a merge.

**And `0x43ADC0` wipes the queue on the way in.** `0x43AE02..0x43AE59` walks `unit+0x5C` and
destroys every existing order that lacks flag bit `0x4` before linking the new one — so with
`shift = 0` an order REPLACES the unit's orders rather than queueing. The applier's `orders`
array therefore only ever takes effect in its last element; recorded in
[scenario format](scenario-format.html).

**The position is copied verbatim, which is why a read-back probe cannot check it**
[BINARY-VERIFIED `0x43A164..0x43A177`]:

```
43a164  mov ecx,[eax]        ; eax = the caller's pos[]
43a169  mov [esi+0x22],ecx   ; Pos.X
43a16c  mov ecx,[eax+0x4]
43a16f  mov [edx+0x4],ecx    ; edx = esi+0x22  -> +0x26
43a172  mov eax,[eax+0x8]
43a175  mov [edx+0x8],eax    ;                 -> +0x2A
```

Three dwords, no scaling and no reordering. So writing the position and reading
`UnitOrders->Pos` back proves only that nothing mangled it — `passed == stored` is a tautology,
and the applier's phase-C "measurement" that concluded whole world units was reading exactly
that tautology.

**What settles the scale, directly.** `0x4815A0` takes a `pos` pointer and turns it into a
map cell [BINARY-VERIFIED, and this is the strongest evidence — it is a consumer, not an
inference]:

```
4815a0  mov eax,[esp+0x4]    ; the pos pointer
4815a5  mov ecx,[eax]        ; pos[0]
4815a7  mov eax,[eax+0x8]    ; pos[2]
4815aa  sar ecx,0x14         ; >> 20 = 16.16 -> world, then / 16 -> a CELL
4815ad  sar eax,0x14
4815b4..c8  bounds-check against main+0x14233 / +0x14237, the map's size IN CELLS
```

`sar 0x14` is only a cell index if the input is 16.16, and the two components it takes are
**0 and 2**, bounds-checked against the cell dimensions — so those are the ground plane and
the middle one is the altitude. `ScriptAction_Type2Index` agrees from the other side: at
`0x43FF5B..0x43FF6F` it reads the three HIGH words (`[esi+0x2]`, `[esi+0x6]`, `[esi+0xA]`) and
does `sar ebx,1` on the middle one before subtracting it from the third — altitude/2 against
depth, the engine's own screen-y term.

**The duplicate-order test at `0x43B006..0x43B029` corroborates it** (on the `shift != 0` path,
so not on ours) [BINARY-VERIFIED]:

```
43b004  mov ebp,[eax]          ; caller pos[0]
43b006  sub ebp,[esi+0x22]     ; minus the candidate order's Pos.X
43b009  add ebp,0x100000
43b00f  cmp ebp,0x200000
43b015  ja  0x43b02b           ; too far -> not the same order
43b017  mov ebp,[eax+0x8]      ; caller pos[2]
43b01a  sub ebp,[esi+0x2a]     ; minus Pos.Z
43b01d  add ebp,0x100000
43b023  cmp ebp,0x200000
43b029  jbe 0x43b034           ; near enough -> merge
```

A tolerance of **±0x100000, which is ±16.0 in 16.16 — one map cell**. In whole world units it
would be ±1 048 576, i.e. the whole map and then some, which is not a tolerance at all; and it
too compares components **0 and 2** and never 1. The order position is therefore the same
`TPosition {x, altitude, depth}` that `UNITS_CreateUnit 0x485F50` takes. **There is no
create/order asymmetry**; `scenario-format.md` §"The coordinate asymmetry" claimed one from
TADR's `ConstructionKickout` and was wrong, and is corrected there.

**Live confirmation** [MEASURED 2026-09-04, `scenarios/shadow-air.json`, Two Continents]: with
whole world units, `1900 >> 16 == 0`, and every ordered unit set off for the map origin — a
Peewee told to `move` to `(1900, 1450)` walked north-west and five aircraft told to fly east
ended stacked in the north-west corner, one of them reading world `(1, 0)`. With the shift the
Peewee stops at `1902` and every aircraft reaches `4200`. `patrol` was broken the same way and
now loops its lane; it had looked like a separate defect and was not.

**Negative results.** `0x43A1F0` (the order destructor, called at `0x43B075`) and `0x4B4F20`
(the free, `0x43B07B`) were not chased further. Three bits of the order flag word
`unit_order+0x42` appear and none is named: `0x40000` picks the `+0x60` unlink head
(`0x43B034`), `0x10000` is OR-ed in at `0x43B068` when the cancelled order is not the one the
caller passed, and `0x4` exempts an order from `0x43ADC0`'s wipe. `0x41074A` reads
`order+0x22`/`+0x2A`/`+0x26` and tests each against zero — an "is the position set" check, in
that order, which is itself consistent with 0 and 2 being the ground plane; what the caller
does with the answer was not chased. `0x43B0B0` is a **different** function
that also calls `0x43ADC0` — do not read the `call 0x43adc0` at `0x43B09D` and the one at
`0x43B120` as the same site.

## `0x458DD0` — the blit-time build-state effect — mapped by us

[MEASURED 2026-09-03, this project — `objdump` of the pristine Steam build, and detoured live.]
The whole nanoframe look — the height-threshold recolour `0x458D30` and the wireframe
`0x458FA0` — is applied at **blit** time to a scratch copy of the composite, every frame
([build-state](build-state.html)). `thiscall(this, GAFFrame* frame, Object3do* obj)`, `ret 8`;
`obj` is at `[esp+8]` on entry. Two early-outs, both `xor eax,eax; ret 8`:

| VA | Bytes | What |
| --- | --- | --- |
| `0x458DD0` | `53 55 8B 6C 24 0C` | `push ebx; push ebp; mov ebp,[esp+0xC]` — `ebp` := the GAFFrame arg. **Six whole bytes, the detour boundary** (a 5-byte steal would split the `mov`) |
| `0x458DDA` | `8b 45 14 / 85 c0 / 75 09` | `frame+0x14` — no depth plane, return 0 |
| `0x458DEA..0x458E06` | `8b 44 24 18 / 8b 50 0c / d9 82 04 01 00 00 / d8 1d c0 d4 4f 00` | `obj+0x0C` → unit, `fld [unit+0x104]`, `fcomp ds:0x4FD4C0` (0.0f) — **`Nanoframe == 0` returns 0**, so this function only ever does anything for a unit under construction |
| `0x458E11..0x458E1F` | `66 8b 8a a8 00 00 00` | `unit+0xA8` (the slot index) into the oscillator maths |

Our detour (`tagpu_owndraw.c`, the third one) replays those six bytes after `popad` — at the
entry `esp`, so the esp-relative `mov` reads what it always read — and takes `xor eax,0; ret 8`
for units the native pass owns, which is the callee's own "did nothing" return. Call sites:
`0x458D0E` (the last `call` in `0x4589C0`, followed by that function's own
`pop edi/esi/ebp/ebx; add esp,0x68; ret 8` — a plain call, not a tail call) for the unit's own
scratch, and `0x459686` for each cargo composite, so a factory's unit-in-progress is covered by
the same skip. Both call sites discard `eax`, so the stub's `xor eax,eax` cannot be observed even
where the real function would have returned 1 (`0x458F88`).

The resume address is **`0x458DD6`**; the six stolen bytes end exactly on an instruction
boundary there.

## `0x459646..0x4596DD` — the cargo loop, and `0x4B90A0` the z-merge — mapped by us

[MEASURED 2026-09-03, this project — `objdump -d -M intel` of the pristine Steam build. Read,
not patched, while fixing "the unit is being built UNDER the lab". The mechanism and the
consequences for our pass are in [build-state](build-state.html) §7, and the whole carry
relationship — what a factory does with the unit on its pad, and what happens when it lets go —
is on [factories](factory-build.html); this is the address-level record.]

**A unit inside a factory is never sorted as its own sprite.** The blit walks the parent's cargo
chain and merges each member's composite *into the parent's scratch*, per pixel:

| VA | What |
| --- | --- |
| `0x459646` | `mov edx,[ebp+0xC]` (the unit) → `mov esi,[edx+0x8A]` — the cargo head; `je 0x4596EB` exits when the chain is empty |
| `0x459657` | `test dword [esi+0x110],0x20000` / `jne 0x4596DD` — **the chain skip**: a member with that bit is stepped over undrawn. Also the loop's re-entry target |
| `0x459670` | `call 0x4586A0(cargo_obj, 1, -1)` — rebuild the cargo composite, **every frame** |
| `0x459686` | `call 0x458DD0(cargo_composite, cargo_obj)` — the build-state effect on the cargo (the section above) |
| `0x45968B..0x4596D3` | the position delta: `cargo+0x6A/+0x6E/+0x72` minus the parent's same three dwords, each taken as its **high word** (the whole-world-unit part) |
| `0x4596D8` | `call 0x4B90A0` |
| `0x4596DD` | `mov esi,[esi+0x8E]` — next in chain; `jne 0x459657` loops |

**`0x4B90A0(srcFrame, dstFrame, sx, sy, dbias)`, `ret 0x14`** — a depth-tested 8bpp paint of one
`GAFFrame` into another ([composite-buffer](composite-buffer.html) has the header layout). The
five arguments come out of the push order at `0x4596B5..0x4596D7`:

- `srcFrame` = `cargoObj3do+0x10`, `dstFrame` = `this+0x10` (the blitter's shared scratch);
- `sx` = `HIWORD(dx)`, `sy` = `HIWORD(dz) − HIWORD(dy)/2` — **the isometric projection of the
  delta**, not a raw dy, so the merge lands the cargo where the camera would put it;
- `dbias` = `HIWORD(dy)`, the pure height delta, applied to the *depth* plane.

The inner loop (`0x4B9130..0x4B9170`), per pixel: skip if the source colour equals the frame's
key byte at `srcFrame+0x8`; otherwise compare `dstDepth` against `srcDepth + dbias` and
**`jg` keeps the destination** — i.e. the source wins on `dstDepth <= srcDepth + dbias`, larger
depth = higher/nearer, the same convention the intra-model rasteriser uses. On a win it writes
the colour and sets `dstDepth = srcDepth + dbias`.

**Two negative results.**

- **`0x4B90A0` has exactly one caller in the entire image** — `0x4596D8`, the line above. A
  disassembly of every section and a scan for the literal find no other call and no pointer to
  it. The cargo merge is the only thing in the game that composites two sprites by depth.
- **The compare and the store disagree about width.** The compare adds `dbias` as a full dword
  to a zero-extended source byte (`0x4B913D..0x4B914D`), but the store re-reads `dbias` as a
  *byte* and does an 8-bit `add cl,dl` (`0x4B914F..0x4B9159`), so a stored depth wraps where the
  comparison did not. Cargo sits within a few world units of its parent, so `dbias` is small and
  this never fires in stock play; it is recorded because it is read, not exercised.

**Why this cost us a bug.** Our native pass first sorted a factory's cargo as an ordinary unit,
which put it on its own tile row — an ARM lab at world y 1072 building a Hammer at 1068 is one
16-unit row apart, four whole depth keys behind the lab, which then covered it at every pixel.
Matching the engine means giving every chain member the parent's row and band and letting the
two models sort against each other by `md`, our intra-model view depth. That **approximates** the
merge rather than porting it: `0x4B90A0` compares a *height* biased by `HIWORD(dy)` and samples
at the projected offset, while `md = (2y − z)/256` is model-local and carries neither term. They
agree while parent and cargo are level, which is every factory pad, and diverge for a cargo whose
origin sits above or below its parent.

## `0x48AB70` — attach and detach one unit to another — mapped by us

[MEASURED 2026-09-03, this project — `objdump -d -M intel` of the pristine Steam build. Read, not
patched, while answering "does a unit walk under the factory in the original too?". Full write-up,
including how a carried unit is drawn: [factories](factory-build.html).]

`0x48AB70 .. 0x48AD2D`, `ret 4`. **One argument, and it is a packed command, not a unit** — which
is what tells you attach/detach is a *simulation* event, replicated by unit id rather than by
pointer:

| Packet | What |
| --- | --- |
| `+0x1` word | child unit id (`0` = none) |
| `+0x3` word | parent unit id (**`0` = detach**) |
| `+0x5` byte | attach point; **`0xFF` = "inside"** |
| `+0x6` byte | two low bits xor'd into `Object3do+0x2E` (`0x48ACE1..0x48ACF0`) |

**Unit id → address**, computed the long way at `0x48AB8A..0x48AB9F`: `id*8 − id` then
`lea eax,[eax+eax*4]` then `lea esi,[ecx+eax*8]` = `*(main+0x14357) + id*0x118`.

**The fields it owns**, all in `UnitStruct`: `+0x86` parent, `+0x8A` head of the carried chain,
`+0x8E` next sibling, `+0xF9` the attach-point byte, and bit `0x20000` of `+0x110`.

**The guards** (`0x48ABC7..0x48AC1D`, all bailing to `0x48AD2A` having done nothing) are the
interesting part, because each is a rule of the game engine: the child must be alive
(`+0x110 & 0x10000000`); **a structure can never be carried** (`+0x110 & 0x20000000` must be
clear); a unit that is itself carrying something cannot be attached (`+0x8A` must be `0`); and
**carrying does not nest** — the parent's own `+0x86` must be `0`.

**`+0x110 & 0x20000` is set iff the attach point is `0xFF`** (`and edx,0xfffdffff` /
`cmp cl,0xff` / `sete al` / `shl eax,0x11` / `or edx,eax`, `0x48AC88..0x48ACA7`). It is a
**"drawn by nobody" flag, not an "is cargo" flag**: the blit's cargo loop skips it at `0x459657`
*and* `DrawUnit 0x45AC20` skips it at `0x45AD43`, so a unit in a transport hold is drawn neither
by itself nor by its carrier. A unit attached to a *piece* — the one on a factory pad — has the
bit clear and is drawn by its carrier, merged through `0x4B90A0`.

**Call sites (4).** `0x455403` in a large command dispatcher (every arm `jmp`s `0x455F50`);
`0x48AB62` from the wrapper `0x48AB40..0x48AB6A` (`ret 0x10`); `0x48B58B` attaching with a real
piece; and **`0x48B5C5` the detach** — child id from `+0xA8`, parent id `0`, point `0xFF`, guarded
on `[edi+0x86] != 0`. Detaching is "attach to nobody", and it happens in one step, so there is no
state in which a unit is still in the chain but positioned away from its carrier.

**Negative results.** `0x47CB00` (no-previous-parent path) and `0x47CB40` (detach path) were not
disassembled, and there is a **second `+0x8E` writer** in `0x47Cxxx` (`0x47CB26`, `0x47CB47`,
`0x47CBA3`, `0x47CBB0`, `0x47CC13`, `0x47CD0F`, `0x47D0B9`) that has not been read. The enclosing
function of `0x48B58B`/`0x48B5C5` was not delimited — `0x48B43C`'s `ret 8` is an early return
inside it, not its end — so the COB opcode that reaches them is unidentified.

## The mouse object, its event ring, and the two ways the cursor gets drawn — mapped by us

[MEASURED 2026-09-04, this project — `objdump -d -M intel` of the pristine Steam build plus live
`tacli peek` on a running game. Established while replacing the composite's cursor-moving hack
with the structural fix (`gpu-status.md` §2.3d). Every VA was read off `objdump` in that session
unless a row says otherwise. This is the layer *below* the cursor chain in the next section: that
one is "which cursor picture", this one is "where the pointer is and who drew it".]

**The object.** `0x4B6220` is exactly `mov eax, ds:0x51FBD0; ret` — the mouse/UI singleton
accessor. So the object is `*(void**)0x51FBD0`. The ring and record helpers below call it at
their head; `0x4C24B0`, `0x4C25E0`, `0x4C67C0` and `0x4C6B10` do **not** — they take the object
(or the surface) as a parameter.

| Offset | What | How established |
| --- | --- | --- |
| `+0x186` | event-ring capacity — the divisor in `idiv` at `0x4C2E44` | disassembly |
| `+0x18A` | event-ring base; entries are 6 dwords (`lea eax,[ecx+ecx*2]` then `*8` = stride 24) | disassembly |
| `+0x18E` | ring **head** (write index), advanced and wrapped at `0x4C2E78..0x4C2E82` | disassembly |
| `+0x192` | ring **tail** (read index), advanced at `0x4C2DB5` | disassembly |
| `+0x196` | the **current mouse record**, 6 dwords, laid out exactly like a ring entry. Written whole by `0x4C2360`, and x/y alone by each of the three drawing polls | disassembly + live |
| `+0x1AE` | cursor hide/nesting counter — `0x4C2870` decrements it and returns early while it is still > 0 | disassembly |
| `+0x1B2` | the current cursor sprite record: size at `+0`/`+2`, read **zero**-extended (`xor edx,edx; mov dx,…`), hotspot at `+4`/`+6`, read **sign**-extended (`movsx`) — so a hotspot may be negative and a size may not | disassembly |
| `+0x1B6` / `+0x1BA` | the position the sprite was last DRAWN at, i.e. position − hotspot | disassembly |
| `+0x1BE` / `+0x1C2` / `+0x1C6` | saved-background rect pointers *[INFERRED]*. **Not one per draw path**: `0x4C2870`, `0x4C24B0` and `0x4C67C0` all use `+0x1BE`, `0x4C25E0` uses `+0x1C2`, and no reader of `+0x1C6` was found in this pass | disassembly |
| `+0x1CE` | a mode word, and **not a simple disable**: `0x4C67C0` returns early when it is **zero**, `0x4C2870` returns early when it is exactly **1**. The two draw paths below are selected by it *[INFERRED]* | disassembly |
| `+0x1D2` | non-zero is required by `0x4C24B0`, `0x4C25E0` and `0x4C67C0` before any of them draws | disassembly |

**The record is 6 dwords, and the fifth is the message id.** Read off TA's own window procedure,
whose `0x200..0x206` jump table at `0x4B60F4` has three arms:

| offset | field | set at (move arm `0x4B5E51`) |
| --- | --- | --- |
| `+0x00` | x | `[esp+0x04]`, the sign-extended lParam LOWORD |
| `+0x04` | y | `[esp+0x08]` |
| `+0x08` | wParam (the key/button flag word) | `[esp+0x0C]` |
| `+0x0C` | a `GetTickCount` stamp (IAT `0x4FC0DC`, KERNEL32), scaled by `[obj+0xE8]` and divided by 1000 | `0x4B5E9C` |
| `+0x10` | **the message id** | `0x4B5EA0` (`mov [esp+0x18],esi`, esi = uMsg) |
| `+0x14` | **1 only for a DOUBLE-CLICK**; 0 for a move and for every single press *and* release — see the arm split below | `0x4B5E90` / `0x4B5EF4` / `0x4B5F40` |

The jump table entries, in order `0x200..0x206`: `0x4B5E51`, `0x4B5EB2`, `0x4B5EB2`, `0x4B5EFE`,
`0x4B5EB2`, `0x4B5EB2`, `0x4B5EFE`. Three arms, and the split is **not** down-versus-up:
`0x4B5EB2` takes all four single presses and releases (0x201, 0x202, 0x204, 0x205) and writes
`+0x14 = 0`; `0x4B5EFE` takes only the two double-clicks (0x203, 0x206) and writes `1`. So
**`WM_MOUSEMOVE` alone goes to `0x4C2360`, which copies its record into `[obj+0x196]`; every
button message goes to `0x4C2E30`, which pushes onto the ring.** That asymmetry is the whole reason a move and a click have to be treated
differently on the input path (`gpu-status.md` §2.3d).

| VA | What it is |
| --- | --- |
| `0x4C2360(rec*)` | copy 6 dwords **into** `[obj+0x196]`. Two call sites: `0x4B5A88` (an init/reset path) and `0x4B5EA4`, the `WM_MOUSEMOVE` arm |
| `0x4C2E30(rec*)` | ring **push**: writes at `[obj+0x18A] + head*24`, advances and wraps head, and **drops the event when the ring is full** (`head+1 == tail`). One call site, `0x4B5F51` — the button arms |
| `0x4C2D60(rec*)` | ring **pop**: copies the tail entry out and advances the tail; when `head == tail` it copies `[obj+0x196]` instead. `ret 4` |
| `0x4C2DE0(rec*)` | the same **without** advancing the tail — a peek. Returns 1 when it took a real ring entry, 0 when it fell back to `[obj+0x196]` |
| `0x4C2340(rec*)` | copy 6 dwords **out of** `[obj+0x196]`, no ring, no poll. 5 call sites |

**The lParam unpack, per arm.** Each of the three arms above unpacks the position with the same
two instructions, and they are the bytes `vpwide` patches to `MOVSX ECX,CX` / `SAR EAX,0x10`:

| arm | unpack at | bytes |
| --- | --- | --- |
| `0x4B5E51` move | `0x4B5E5F` | `81 E1 FF FF 00 00` `C1 E8 10` — `AND ECX,0xffff` + `SHR EAX,0x10` |
| `0x4B5EB2` single press or release | `0x4B5EC0` | the same nine bytes |
| `0x4B5EFE` double-click | `0x4B5F0C` | the same nine bytes |

`LOWORD`/`HIWORD` is zero-extending, so a client x of −20 arrives as 65516 and the event is lost
— see `gpu-status.md` §2.3b for why that is half the addressable ring at zoom < 1.

**`0x4C6B10` — the surface clip-rect store, and it clamps nothing.**

```
4c6b10  mov eax,[esp+4] ; l          add ecx,0x1c            ; this + 0x1C
4c6b14  mov edx,[esp+8] ; t          mov [ecx],eax           ; +0x1C  l
4c6b18                               mov [ecx+4],edx         ; +0x20  t
4c6b1d  mov eax,[esp+0xC]; r         mov [ecx+8],eax         ; +0x24  r
4c6b24  mov edx,[esp+0x10]; b        mov [ecx+0xC],edx       ; +0x28  b
4c6b2e  ret 0x10                                             ; __thiscall(self,l,t,r,b)
```

Four dwords into the surface's clip rect at `+0x1C..+0x28` and not one bound check —
`SurfaceCreateNamed` initialises the same field to `(0, 0, w-1, h-1)`, which is the bound a
caller is expected to respect.

**Six call sites, and `vpwide` redirects three.** `0x468D85`, `0x46964F` and `0x469F95` are the
`DrawGameScreen` sites that feed it the viewport rect `main+0x37E27`, so those are the ones that
would hand it a widened rect; they are redirected and clamped (`gpu-status.md` §2.3b). The other
three are `0x495CAC`, `0x4A20A1` and `0x4A22DF`, and **none of them is handed the viewport
rect** — settled by two independent disassembly reads during the G13m landing review. `0x495CAC`
builds its argument block at `0x495C91..0x495CA9` from a loop accumulator, a literal `0` for `t`,
and map dimensions (`main+0x1423B`/`+0x1423F`), on a **stack-local** surface (`lea ecx,[esp+0x9c]`);
its enclosing function `0x495A30` does read `main+0x37E27`/`+0x37E2B`, but into other locals.
`0x4A20A1` and `0x4A22DF` are a save/restore pair around `0x4C6AE0`, which is the matching clip
**getter** — `add ecx,0x1C` then four dwords copied OUT (verified here). Only
`0x468D85`/`0x46964F`/`0x469F95` copy `main+0x37E27` immediately before the call, and those are
the three that are redirected.

**Still open:** `0x495A30` calls `DrawGameScreen 0x468CF0` in a loop while driving the eye, so it
renders *under* whatever rect is in force. Whether a widened rect can reach an engine drawer that
way was not settled.

**`GetCursorPos` is the IAT slot `ds:0x4FC2E4`, reached from six places, and only three of them
move the cursor.**

| Site | Return addr | Draws? | What it is |
| --- | --- | --- | --- |
| `0x4C28AA` | `0x4C28B0` | **yes** | inside `0x4C2870`, the hide-counter path: poll → `[obj+0x196]` → `+0x1B6/+0x1BA` = answer − hotspot → save background `0x4C6B70` → blit `0x4B7F90` |
| `0x4C24DE` | `0x4C24E4` | **yes** | inside `0x4C24B0`, same shape, saved rect at `+0x1BE` |
| `0x4C2610` | `0x4C2616` | **yes** | inside `0x4C25E0`, same shape, saved rect at `+0x1C2` |
| `0x41CEE7` | `0x41CEED` | no | inside the scroll poll: **only** the off-screen warp-back, see below |
| `0x4C2318` | — | no | a bare `GetMousePos(int*,int*)` wrapper (`0x4C2310`, `ret 8`). **DEAD: zero `call` sites and the literal `0x004C2310` does not occur anywhere in the image** |
| `0x49F7CA` | — | no | a `jmp` thunk to the same import. **DEAD by the same two tests** |

All three drawing polls take the position from **their own answer**, never from `[obj+0x196]`.
That matters, because there is a fourth draw that does the opposite.

**THE SECOND DRAW: `0x4C67C0(mouseObj, surface)`, which blits from the RECORD and never polls.**
It early-outs unless `[+0x1CE]`, `[+0x1D2]` and `[+0x1B2]` are all non-zero, then computes
`+0x1B6/+0x1BA` from `[obj+0x196]` − hotspot, saves the background through `0x4C6B70` and blits
through `0x4B7F90` — the same blit the polling paths use. Two call sites, both in the surface
present machinery: `0x4C641B` and `0x4C6544`, **both inside the same function `0x4C63A0`**
(which runs to its `add esp,0xF4; ret` at `0x4C67BA` and has **44 callers** of its own — 20 in
`0x41Fxxx`, the rest spread over `0x420xxx`–`0x49Fxxx`).

**So `[obj+0x196]` is not merely "the last mouse record" — it is a position the cursor can be
drawn at.** Whatever wrote it last decides where the sprite appears on the next present: a
`WM_MOUSEMOVE` through `0x4C2360`, or the last drawing poll. Measured, 1920×1080 at zoom 0.25×
with `fake_GetCursorPos` answering the true pointer while the move message still carried the
transformed `u`: the sprite tracked `u` across the frame at 4× the pointer's speed and off the
left edge. `0x4C2380` — which the previous pass listed as the record-drawing path — is **DEAD**
(zero `call` sites, literal absent); `0x4C67C0` is the live one.

**`main+0x2C76` — the record the game actually acts on.** Filled once per mouse dispatch by the
routine that ends at `0x499A2A`:

```
0x499982  PeekMouseEvent(&A)                       ; 0x4C2DE0, into [esp+0x8]
0x49999C  PeekMouseEvent(&B)                       ; 0x4C2DE0, into [esp+0x20]
0x4999AB  if (A.msg == B.msg)  GetMouseEvent(main+0x2C76)     ; 0x4C2D60 — pops
          else if (A.msg == 0x202 || A.msg == 0x205) main+0x2C76 = A
          else                                       main+0x2C76 = B
0x499A1C  call [main+0x391F5]                      ; the per-input-mode handler
```

`0x499200` (input mode 6, in-game — see the next section) then copies those 6 dwords to its own
stack and passes the copy to `0x498DA0`. **The copy is not what the readers after that call
read**: `GetUnitAtMouse 0x48CD80` at `0x499278` and the mouse routing test at `0x469DE1` both
load `main+0x2C76`/`+0x2C7A` directly. Anything that corrects the mouse point inside a
`0x498DA0` redirect must therefore write the field as well as the copy.

Readers of `main+0x2C76` found in the image: `0x41635D`, `0x419BF0`, `0x41A4B2`, `0x41CCAF`,
`0x41CD63`, `0x41D101`, `0x469DE7`, `0x48CD91`/`0x48CD97`, `0x496490`, `0x498D16`, `0x499210`.
The only writers are the three stores above. `0x419BF0` and `0x41A4B2` are inside the order
dispatchers `0x419BE0`/`0x41A490`, so this record does not only drive hover — a wrong value here
becomes a wrong *order*.

**The scroll poll is `0x41CE90`, not `0x41CF10`.** *[CORRECTION: `gpu-status.md` §2.3c named
`0x41CF10`, which is not an instruction boundary — `0x41CF0E` is `lea ebp,[esi+0x64]`.]* One
caller, `0x496976`. It reads the mouse position with `0x4C2340` — i.e. out of `[obj+0x196]`, not
from a fresh poll — into `[esp+0x1C]`/`[esp+0x20]`, and then tests, against the SCREEN dimensions
at `main+0x37E1F`/`+0x37E23`:

```
0x41CF8B  x == 0            -> eye.x -= step        (or hotkey 0xF4, tested 0x41CF7A)
0x41CFC8  x == screenW - 1  -> eye.x += step        (or hotkey 0xF6, tested 0x41CFA5)
0x41CFE9  y == 0            -> eye.y -= step        (or hotkey 0xF5, tested 0x41CFCE)
0x41D021  y == screenH - 1  -> eye.y += step        (or hotkey 0xF7, tested 0x41CFFF)
0x41D054  call 0x41C3C0                             ; the eye clamp
```

The `GetCursorPos` at `0x41CEE7` is **not** that test. It runs first and only handles the pointer
having gone 1..100 px *past* the screen: `(x >= screenW || y >= screenH) && x < screenW+100 &&
y < screenH+100`, and then — if `GetFocus` (IAT `0x4FC2E0`) matches `[obj+0x40]` — it overwrites
`[esp+0x1C]`/`[esp+0x20]` with the polled position clamped to `screenW-1`/`screenH-1`. So the
edge scroll fires off whatever `[obj+0x196]` holds, which is why answering the true pointer at
the three drawing polls is what makes the right edge work at zoom > 1 (`gpu-status.md` §2.3c).

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

## The UI surfaces and their writers — mapped by us (Phase E, G15a, 2026-09-07)

*Everything the engine draws into an 8bpp surface on the UI path — the shell screens, the
in-game side panel and its option screens, the bars, the minimap, chat and the popups — and
the surfaces those pixels land in. Established for [the GL UI renderer](gui-renderer.html):
static reading of the pristine binary (objdump, md5 `8e74a1dffa1f5988624c52048f5b20cd`),
then MEASURED by a census that observes every leaf below and diffs every surface at every
flip (`tagpu_gui_hook.c`, `tagpu_gui.on=census`). The census explains 100 % of the pixels
that change on the presented surface across the whole screen inventory (§ "What the census
measured"); the leaves below are therefore the complete set for the UI, and the exceptions
are named.*

### The graphics globals block — `globals = *(void**)0x51FBD0` [VERIFIED]

`0x4B6220` is `mov eax,[0x51FBD0]; ret`. The fields the UI path uses:

| Offset | Meaning | Evidence |
|---|---|---|
| `+0x44` | non-zero ⇒ GDI/windowed present; `+0x50..+0x7F` is then an inline OFFSCREEN over the DIB | `0x4C5EA4..0x4C5EC3`, `0x4C63C0` |
| `+0x88` | `IDirectDrawSurface*` primary (`Lock` vtbl+0x64 at `0x4C6509`, `Unlock` vtbl+0x80 at `0x4C6595`, `Flip` vtbl+0x2C at `0x4C6682`) | `0x4C64E7`, `0x4C658A`, `0x4C6676` |
| `+0x8C` | `IDirectDrawSurface*` used when there is no system back buffer | `0x4C5EC6..0x4C5F92` |
| `+0x98` | OFFSCREEN\* written by `0x4C61F0(surface)`; read **only** on the flip's `DDERR_SURFACELOST` restore paths (`0x4C65E3`, `0x4C675B`). Not the normal source | `0x4C61F0..0x4C61FF` |
| `+0xA0..+0xAF` | clip rect copied into the context `0x4C5E70` builds over a locked DD surface | `0x4C5F53..0x4C5F6A` |
| **`+0xBC`** | **OFFSCREEN\* system-memory back buffer — what every flip presents and every NULL-context blit draws into.** Written by `0x4C69A0(surface)` together with `+0xDC = 1` | `0x4C69A0..0x4C69B9`; read at `0x4C6405`, `0x4C6485`, `0x4C5E86`, `0x4C68B5` |
| `+0xC0` / `+0xC8` | blend LUT / shade table (the ALP remap) | `0x4B8665`, `0x4B847A` |
| `+0xD4` / `+0xD8` | screen width / height (`0x4B6700` / `0x4B6710`) | `0x4B6700..0x4B671B` |
| **`+0xDC`** | 1 ⇒ `+0xBC` is valid; set by `0x4C69A0`, cleared at `0x4C638C` | |
| `+0xF0` bit 1 | DirectDraw present (clear ⇒ GDI); bits 5 / 7 gate `0x4B8500` / `0x4B8310` | `0x4C63B8`, `0x4B8519`, `0x4B8329` |

`0x4C69A0` has 28 callers; **`0x468D30` inside `DrawGameScreen` makes `*(main+0x37E1B)` the
back buffer every frame**, and `0x467D7F` (the HUD painter) does the same. **In the shell the
back buffer is the startup `"OFFSCREEN" 640×480`**, a different object; the census confirmed
both by address (`main+0x37E1B == *(globals+0xBC) == 0x04490020` in a 1024×768 game).

### The OFFSCREEN object [VERIFIED, `0x4C69F0`]

```
+0x00 width   +0x04 height   +0x08 pitch (= width)   +0x0C pixel base (= this + 0x30, inline)
+0x10 0x2710  +0x14 -1       +0x18 s16 originX  +0x1A s16 originY   (0 on create; the GAF
                                                  hotspot when 0x4B8A80 built the header
                                                  over a GAF frame)
+0x1C..+0x28  clip L,T,R,B, INCLUSIVE, (0,0,w-1,h-1) on create
+0x2C         flags: bit0 = heap object, freed by 0x4D85A0
+0x30..       pixels, w*h bytes;  allocation = w*h + 0x30 via 0x4D83B0(tag, size)
```

- **`SurfaceCreateNamed 0x4C69F0(const char* tag, int w, int h)`** — `stdcall`, `ret 0xC`,
  returns the object. Prologue `53 56 8B 74 24 10`. 18 callers; the GUI's are `0x4A907C`
  (the screen's own surface, tagged with the screen's name) and `0x4A90B5` (its `"SAVE UNDER"`
  snapshot); `0x498407` creates the game offscreen `main+0x37E1B`; the minimap's are
  `0x466823`, `0x466881`, `0x4669CF`, `0x4669FA`. **The tags name the surfaces**, and a
  session's inventory reads (MEASURED): `"OFFSCREEN" 640×480` (shell) and `1024×768` (game),
  one `"<SCREEN>.GUI"` per pushed screen (`MAINMENU.GUI 640×480`, `ARMCOM1.GUI 128×352`,
  `PREFS.GUI 128×354`, **`VISUALRT.GUI 278×354`**, `TALK.GUI 512×33`, …), a `"SAVE UNDER"` per
  screen, `"SAVEMOUSE 1..3"`, `"FLIPSURFACE" 128×352`, `"BKUPSURFACE" 300×480`, and one
  `"bitmaps\<name>.PCX" 640×480` per shell background loaded.
- **`SurfaceFree 0x4C6AC0(OFFSCREEN*)`** — `stdcall`, `ret 4`: `if (p && p[+0x2C] & 1)
  0x4D85A0(p)`. Prologue `8B 44 24 04 85 C0`. 23 callers; the GUI's are `0x4A9537`
  (`panel+0xB8`) and `0x4A9549` (`panel+0xBC`) in the teardown arm.
- **`SurfaceFill 0x4C6890(surface, colour)`** — `stdcall`, `ret 8`: fills `h·pitch` bytes at
  `+0xC`; `NULL` ⇒ the back buffer. Prologue `83 EC 64 53 55 56 57`.
- **`GetContext 0x4C5E70(OFFSCREEN* out)`** — `stdcall`, `ret 4`, prologue `83 EC 6C 56 57`
  (then `E8`): the NULL-context path every blitter takes. Arm 1, `[globals+0xDC] != 0` →
  `rep movs 0xC` from `*(globals+0xBC)`, return 1 — **the arm taken in game and in the shell**;
  arm 2 the GDI block; arm 3 locks `[globals+0x8C]`. `0x4C5FA0(ctx)` releases arm 3 only. 51
  call sites each. **So a blitter given `ctx == NULL` draws into the system back buffer.**
- `0x4C6AE0 GetClipRect` — `thiscall(ecx = ctx, RECT* out)`, `ret 4`, four dwords from
  `ctx+0x1C`. `0x4B7E60 ClipRectPair(dst, src, clip)` — `stdcall`, `ret 0xC`.

### `FlipOffscreenToPrimary 0x4C63A0` — no arguments, and where the cursor goes [VERIFIED]

`void (void)`, plain `ret`; prologue `81 EC F4 00 00 00` (6, or 10 with the four pushes). 44
`call`s and 3 tail `jmp`s (`0x4257BA`, `0x425B7B`, `0x45CFB8`): `0x46A3DB` in DrawGameScreen,
`0x49FA32` in the shell's modal loop `0x49F9C0`, `0x467E41` in the HUD painter, and 41 more.
The DirectDraw arm (`0x4C6475`) requires `[globals+0xDC]`, takes **`edi = *(globals+0xBC)`** as
the source, checks it against the screen size, locks the primary, builds a context over it at
`[esp+0x38]`, **draws the cursor into the back buffer with `0x4C67C0(globals, edi)`**, copies
back buffer → primary with `0x4CBBE0(&ctx, edi, 0, 0)`, **restores the cursor's background
with `0x4C6B70(edi, [globals+0x1BE], [globals+0x1B6], [globals+0x1BA])`**, and unlocks. So the
back buffer holds the cursor only between two calls inside the flip: at the flip's entry it
does not, and a diff taken there never sees the cursor. The surface-lost arm re-copies
`[globals+0x98]`; the GDI arm copies into `globals+0x50` and presents with `StretchDIBits`
[INFERRED from the IAT slot]. **MEASURED: the shell flips about 5 000 times a second** on this
machine (31 678 flips in the first 6 s of a launch); in game once per `DrawGameScreen`.

### The GUI is retained: `GUI_StageUpdateDraw 0x4A81E0` builds, `0x4AB0B0` blits [VERIFIED]

**`0x4A81E0(GUIInfo* gi, int flags)`** — `stdcall`, `ret 8`; prologue
`8B 44 24 04 81 EC C0 03 00 00` (10). The dispatcher at `0x4A9176` (table `0x4A962C`) is
inside it. Flags: `0x1` **build** (surfaces are created only under it), `0x2` **teardown**,
`0x40` redraw, `0x8` buttons only (`test …,0x48` at `0x4A91AD`), `0x4/0x40/0x80` background
variants, `0x20` skip the snapshot, `0x100/0x1000` recentre. Observed callers: `push 2` in
`GUI_Pop 0x4A968E`, `push 1` in `TA_DialogBox_fn 0x4ABD90` (`0x4AC01B`, `0x4AC198`),
`esi|0x40` at `0x494210` (the in-game panel loader). 76 sites, **none in DrawGameScreen**.

**The build sequence (`0x4A905E..0x4A9135`, read 2026-09-07):**

```
panel+0xBC = 0x4C69F0(panel->name /* or 0x509920 */, w, h)   ; the screen's own surface
0x4C6B70(panel+0xBC, NULL, -xpos, -ypos)                     ; the screen UNDER it, copied in
unless flags&0x20:
  panel+0xB8 = 0x4C69F0("SAVE UNDER" @0x509914, w, h)        ; and kept aside…
  0x4C6B70(panel+0xB8, panel+0xBC, 0, 0)                     ; …for the teardown restore
background: 0x4C6B70(panel+0xBC, *(GUIMEM+0x24), 0, 0)        ; the PCX background surface
        or  0x4B0230(gi, 0, panel+0xC4)                      ; or the tiled texture
then the gadget loop 0x4A9135..0x4A942E over EVERY gadget 1..totalgadgets,
skipping only active == 0 — there is no per-gadget dirty flag
```

Every handler draws into `[panel+0xBC]` with that object as its context (`ebx=[panel+0xBC]` at
`0x4A6056` in the button handler, and so on). Teardown (`flags&2`, `0x4A950A..`) copies
`panel+0xB8` back to the screen, frees both, and `GUI_Pop 0x4A9660` frees the GUIMEMSTRUCT.
**`*(GUIMEM+0x24)` is the screen's background PCX decoded into a surface** — filled by the
loader, not by any draw leaf (MEASURED: 307 072 unexplained bytes on `MAINMENU.GUI`'s
`03D51178 640×480`, zero ops), and read only as a copy source.

| `id` | handler | `ret` | prologue (steal) | draws through |
|---|---|---|---|---|
| 1 button | `0x4A5F40` | 8 | `81 EC D8 00 00 00 53 55` (8) | `0x4B7F90(ctx, frame, x+HotX, y+HotY)` at `0x4A61BE`, `DrawText 0x4A50E0`, `0x4BE950`, `0x4B8310`; sets `TheActive_GUIMEM+0x14 = 1` at `0x4A5F5E` |
| 2 listbox | `0x4A1B40` | 8 | `81 EC BC 00 00 00 53 55` (8) | `0x4C6D20` at `0x4A1C01`, `0x4B0230(gi, idx, 0)`, rows via `0x4A23B0..0x4A2BE0` (`0x4B7F90` ×13), clip save/restore `0x4A2068` |
| 3 textfield | `0x4A4D70` | 8 | `83 EC 18 8B 54 24 1C` (7) | `0x4BF6F0`, `0x4C6D20`, `0x4BE950`, DrawText |
| 4 slider | `0x4A3EF0` | 8 | `8B 44 24 04 83 EC 08` (7) | nothing itself; `0x4A2580` paints |
| 5 label | `0x4A56B0` | 8 | `81 EC AC 00 00 00` (6) | `0x4BF6F0`, `0x4C14F0` / DrawText, `0x4BE950` |
| 6 surface | `0x4A4980` | 8 | `83 EC 50 53 55` (5) | **`0x4C7580`** (the textured-triangle stamp, below), `0x4B7F90`, `0x4BF6F0` |
| 10 | `0x4A4C90` | **0xC** `(gi, idx, flags)` | `8B 54 24 04 83 EC 10` (7) | `0x4BE950` ×2 |
| 11 picture / panel bg | `0x4B0230` | **0xC** `(gi, idx, GAFFrame*)` | `83 EC 24 8B 54 24 2C` (7) | NULL frame: the texture from `0x4A18C0` tiled with `0x4C6B70(panel+0xBC, tex, x, y)` + bevel `0x4B0160`; else `0x4B7F90` |
| **12** | **`0x4A5E50`** — not `0x4A5F40` as [GUI gadgets](gui-gadgets.html) §3 said; corrected there | | | |
| 13 timer | `0x4A4660` | 8 | `83 EC 24 53 55` (5) | `0x4C5E70(ebx)` over `[panel+0xBC]` or `[gi+0xCD2]`, then `0x4BF6F0`, `0x4B7F90`, DrawText, `0x4C5FA0` — i.e. into the back buffer |

`DrawText 0x4A50E0(ctx, str, x, y, maxW, shade)` — `stdcall`, `ret 0x18`: with no GUI font
set, `0x4C14F0(ctx, s, x, y, -1)`; else per character `0x4B7F30([[0x51FBA4]+0x14]+0xC, ch)` →
`0x4B7F90(ctx, glyph, x, y)` (`0x4A5185`) or `0x4B8310` (`0x4A5191`). **So GUI text is GAF
glyph blits, and `0x4CCF60` is only reached through `DrawTextCustomFont 0x4C14F0`** (its two
callers `0x4C16D4`, `0x4C1744`); MEASURED, in-game option screens still take that path for some
labels (`text 1572` ops on `PREFS.GUI`).

**How a screen reaches the frame: `0x4AB0B0(GUIMEMSTRUCT*, OFFSCREEN* dst, RECT* dirty)`** —
`stdcall`, `ret 0xC`, prologue `83 EC 10 57 8B 7C 24 18` (8): recurses bottom-to-top over
`per_active`, and blits **`0x4C6B70(dst, panel+0xBC, xpos, ypos)`** (`0x4AB158`) only if
`mem+0x14 == 1` (cleared after) or `0x4B67D0(&panelRect, dirty)` [INFERRED name] says the rect
overlaps. Two callers: `0x49FA28` in the **shell modal loop `0x49F9C0(gi, untilScreen)`** —
`{pump messages; 0x4A9FD0(gi); 0x4AB0B0(top, NULL, NULL); 0x4C2870; 0x4C63A0}` — and
`0x4AB182` in the thunk **`0x4AB170(gi, ctx, dirty)`** (`ret 0xC`, prologue
`8B 44 24 0C 8B 54 24 04 8B 4C 24 08`), called once per frame from DrawGameScreen at
`0x46A303` with `(main+0x519, &[esp+0x34], main+0x37E27)` after `0x4C69C0(&ctx)` resets the
clip.

### The leaves — every function that writes UI pixels [VERIFIED; MEASURED complete]

| VA | name | convention, `ret` | arguments | destination and box | prologue stolen |
|---|---|---|---|---|---|
| `0x4B7F90` | `CopyGafToContext` | stdcall `0x10` | `(ctx, GAFFrame*, x, y)` | `{x−HotX, y−HotY, +w−1, +h−1}`, hotspot **signed** (`movsx` at `0x4B802C/0x4B803F`), clipped by `0x4C6AE0`+`0x4B7E60`; sub-frames route to `0x4B8500` when `+0xB` is set; leaf `0x4CBE70` (raw), `0x4CC51D` (RLE) | `81 EC 94 00 00 00` (6) |
| `0x4B8500` | `AlphaCompsteBuf2OFFScreen` | stdcall `0x10` | same | same, blended through `[globals+0xC0]`; 24 callers, none in the GUI | `81 EC 94 00 00 00` (6) |
| `0x4B8310` | the blit `DrawText` takes under `globals+0xF0` bit 7 | stdcall `0x10` [INFERRED from the call site] | same shape | same | `81 EC 94 00 00 00` (6) |
| `0x4B8150` | opaque GAF blit | stdcall `0x10` | `(ctx, GAFFrame*, x, y)` — a **frame**, not a descriptor | leaf `0x4CBDD1`, no key; 4 callers, **all terrain** (`0x484110`, `0x48415C`, `0x484228`, `0x484274`) — not a UI leaf | |
| `0x4C6D20` | descriptor blit | stdcall `0x10` | `(ctx, desc {w,h,stride,pixels}, RECT* src, RECT* dst)` | `*dst`; GUI callers `0x4A1C08` (listbox), `0x4A4EC0` (textfield); `0x4C6DC0` is the keyed twin with no callers | `8B 44 24 04 83 EC 30` (7) |
| **`0x4C7580`** | **textured-triangle stamp** (`GAF_DrawTransformed` [CORPUS]) | stdcall `0x10` | `(ctx, src, int xy[6], int uv[6])` — three screen vertices and their texture coordinates, **MEASURED** `(214,94)(233,94)(233,113)` with uv `(1,1)(31,1)(31,31)` | the vertices' bounding box; **the in-game option screens' wide dark backdrop right of the 128-px panel is drawn as these** (13–37 per build), which is why that region has slanted edges | `B8 8C 7D 00 00` (5, the stack probe) |
| `0x4CCF60` | glyph blitter | cdecl, 9 args | `(base, pitch, font, str, x, y, fg, bg, transparent)` | row `y − (s8)font[2]`, width the sum of `font[off]` per glyph, stops at `\0` **or `\n`**; 2 callers, both in `0x4C14F0` | `55 8B EC 83 C4 F0` (6) |
| `0x4BE950` | `DrawLine` | stdcall `0x18` | `(ctx, x0, y0, x1, y1, colour)` | bbox after `0x4BEA20`; 83 callers | `83 EC 30 56 8B 74 24 38` (8) |
| `0x4BF6F0` | `DrawBar` | stdcall `0xC` | `(ctx, RECT*, colour)` | the rect, inclusive, via `0x4BF620` + `0x4CCDEA`; 47 callers | `83 EC 40 8B 44 24 48` (7) |
| `0x4BF8C0` | `DrawTranspRectangle` | stdcall `0xC` | `(ctx, RECT*, colour)` | hollow rect; 12 callers incl. the minimap view box `0x466B5E`, the HUD `0x467F6C` | `83 EC 68 53 56 57` (6) |
| `0x4BF7B0` | the focus rectangle | stdcall `0xC` | `(ctx, RECT*, colour)` | drawn last by `GUI_StageUpdateDraw` via `0x4A16F0(gi, idx, 8)` | `83 EC 30 53 55 56 57` (7) |
| `0x4BF4D0` | framed box | stdcall `0xC` | `(ctx, RECT*, colour)` | three clipped fills (`0x4BF620` ×3, `0x4CCDEA` ×2); **what `DrawPopupF4Dialog 0x4948E0` draws its border with** (×3) | `83 EC 40 53 55 56 57` (7) |
| `0x4C6890` | `SurfaceFill` | stdcall `8` | `(surface, colour)` | the whole surface | `83 EC 64 53 55 56 57` (7) |
| `0x4C6B70` | surface → surface | stdcall `0x10` | `(dst, src, x, y)` | `0x4CBBE0(dst, src, x − (s16)src[+0x18], y − (s16)src[+0x1A])`; `dst == NULL` ⇒ the back buffer, `src == NULL` ⇒ the screen; 23 callers — the GUI blit `0x4AB158`, the build's snapshot `0x4A9098`, `0x4A90CC`, `0x4A9111`, the teardown `0x4A952B`, the picture tiler `0x4B02DF`, the minimap `0x466B44`, cursor save/restore | `83 EC 30 56 8B 74 24 38` (8) |
| `0x4CBBE0` | `CopyScreenContext` | cdecl | `(dst, src, x, y)` | the whole `src` at `dst+0xC + y·pitch + x`, clipped by `dst+0/+4` only — **reads neither clip rect**; 17 callers: three in the flip, two in `0x4C6B70`, **ten in cursor code** (the `SAVEMOUSE` buffers are written through it directly) | `55 8B EC 83 C4 E4` (6) |

Not drawers, checked because the popups call them: `0x47F1A0` (helpers `0x47F0C0`, `0x44FDB0`,
`0x451DF0`, …, no pixel write), `0x4B6560` (`jmp [0x4FC0DC]`, an import thunk), `0x4A5030`
(text measure, calls `DrawText`). `0x4C69A0`, `0x4C61F0` and `0x4C5FA0` begin with `E8` and
cannot be prologue-detoured without relocating the call.

### The popups, chat and the HUD painter [VERIFIED call graphs]

- `DrawPopupF4Dialog 0x4948E0`: `DrawText 0x4A50E0` ×5, `0x4BF4D0` ×3 (the frame), `0x4C7580`
  ×1, `0x4B7F30`. `DrawPopupButtomDialog 0x4689C0`: `DrawText` ×3, `0x4B7F90` ×1, `0x4C6AE0`.
  `DrawChatText 0x464060`: `DrawText` ×1, `0x467C00` ×1 (a message-box painter that also calls
  `0x4C6890`, `0x4C69A0` and the flip itself), `SetFont`/`SetTextColors`.
- **`0x467D70` the HUD panel painter**, `void (void)`, one caller `0x49842A`, prologue
  `A1 E8 1D 51 00` (5): `esi = *(main+0x37E1B)`; `0x4C69A0(esi)`; `0x4C6890(esi, 0)`; three
  side-specific GAF frames (`main + side·4 + 0x1481F / 0x14833 / 0x14847`, side from
  `[[main+0x1B8A+player·0x1A2]+0x95]`) via `0x4B7F30(seq, 0)` → **`0x4B7F90(esi, f, HotX+0x81,
  HotY)`** (top bar), `(HotX+0x81, HotY+screenH−0x20)` (bottom bar), `(HotX, HotY)` (side
  panel); then `0x4C63A0()`.
- The `LIGHTBAR` slide `0x45FFB0`: `0x4B8D40`, `0x4B7F30`, `0x4B7F90`, `0x47F1A0`.

### The minimap, located [VERIFIED]

`main+0x1426B` (`TED_GENERATED_PIC`) is consumed once, at `0x46684F` in
**`BuildMinimapSurface 0x466780`** (no args, prologue `83 EC 40 53 8B 1D E8 1D 51 00`): it
fits a 126-px box (`main+0x142EB/+0x142ED` size, `+0x142E7/+0x142E9` offsets), creates
`main+0x142E3 = 0x4C69F0(0x5074F8, w, h)` and scales the picture into it (`0x4B8AE0` + `0x4B95A0`
[INFERRED stretch]). Three surfaces: `+0x142E3` the scaled map; `+0x142DF` the fog composite,
rebuilt by `0x466C20` with **direct byte writes** (callers `0x465572`, `0x48191A`); `+0x142DB`
the composite plus radar dots, rebuilt by `0x466DC0` (`0x4C6B70([0x142DB],[0x142DF],0,0)` then
unit dots through `0x4B7F90` of `main+0x147DF/+0x147E3`, `0x4C0070` arcs, `0x4BEE60 DrawPoint`;
callers `0x465072`, `0x48191F`). **Per frame, `DrawMinimap 0x466B00(ctx)`** (`stdcall`, `ret 4`,
prologue `8B 0D E8 1D 51 00`, gated on `main+0x142F1 & 2`) does
`0x4C6B70(ctx, [main+0x142DB], main+0x142E7, main+0x142E9)` at `0x466B44` and the view box
`0x4BF8C0(ctx, main+0x142CB, main+0xDD9)` at `0x466B5E`; **one caller, `0x46961F` in
DrawGameScreen, with the game offscreen's context.** The minimap never goes through the GUI
surfaces. The map loader builds `main+0x1426B` at `0x483900..0x483936` as a GAF frame
(`0x4B8DA0(0x508B6C, w, h)`, filled via `0x4B8A80` + `0x4B7F90`) and frees it at
`0x483DF3`/`0x483E0B`.

### What the census measured [MEASURED 2026-09-07, `tools/uiwalk.py`, 1024×768]

With every leaf above observed and every surface diffed at every flip (`CENSUS_MS 5`, so at
most 200 diffs a second against the shell's ~5 000 flips), across the inventory — `MAINMENU`,
`SINGLE`, `SKIRMISH`, `SELMAP`, `STARTOPT`, `VISUALS`, and in game `ARMMAIN2`, `ARMCOM1/2`,
`ARMOPT`, `PREFS`, `VISUALRT`, `TALK` (chat), the F4 popup — **3 710 035 pixels changed on the
presented surface and 0 were unexplained**; a screen transition in the shell is 614 400
changed pixels (the whole 640×480) explained by ~50 000–150 000 GAF blits and a few thousand
lines, rects and copies. The unexplained changes on *other* surfaces, all outside the frame:

- the PCX backgrounds decoded into their `"bitmaps\…PCX"` surfaces by the loader (~300 000
  bytes each, zero ops) — assets, read only as copy sources;
- the `"SAVEMOUSE"` buffers, written by `0x4CBBE0` directly from cursor code;
- **the startup `"OFFSCREEN" 640×480` keeps being written in game** by an unobserved path at
  in-game screen builds (185 942 bytes when `ARMCOM1` appears, 43 869–83 573 in rows 226–479
  on `ARMOPT`/`TALK`/`ARMCOM1` pages), and **`"FLIPSURFACE" 128×352`** is filled whole
  (43 519 bytes) when `PREFS` opens — neither is ever presented, and anything reaching the
  frame from them goes through an observed copy [OPEN: the writer of each].

The census also fixed three claims elsewhere in this wiki: the nine `0x46B900` calls in
DrawGameScreen's tail are the **debug profiler bars**, not "side panel / minimap"
([UI markers](ui-markers.html) §4); the minimap is not `0x48CC30`/`0x46A430`
([frame composition](frame-composition.html) §1); and `id 12` dispatches to `0x4A5E50`.

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
