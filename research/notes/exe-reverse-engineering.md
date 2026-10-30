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

## The engine map, by subsystem

What we have mapped of the engine lives in one page per subsystem; this page keeps the survey of what others published. The [address index](engine/address-index.html) lists every function with an entry and where it is; `grep -rn 0x… research/notes/engine/` finds every mention.

- [Memory, lifetimes and teardown](engine/memory.md) — The allocator, the `main` block, the per-map arrays, the destructors and the level teardown.
- [Process, threads, files and registry](engine/process.md) — Start-up, the crash filter, the threads and the load flags, DirectDraw, the archives, the registry, sound and the movie player.
- [The simulation step and its clock](engine/sim.md) — The sim tick, its rate, the step's phases and counts, and how a level starts and ends.
- [Units: records, creation, death and transport](engine/units.md) — The unit and player records, creation, the unit cap, build lists, death, transport and attachment.
- [Weapons, damage and effects](engine/weapons.md) — Weapon decisions and IDs, damage, veterancy, area damage, projectiles, the effect pools and the nano spray.
- [The map: terrain, features, footprints, fog and sight](engine/map.md) — The terrain pass, features and wrecks, the wind, the grid stamp and footprint contest, the fog grids and line of sight.
- [Orders and the AI](engine/orders.md) — The order module, order fallbacks, resurrect, the AI's order activity and resource placement.
- [Drawing the frame: blits, rasterisers, shadows and markers](engine/render.md) — The offscreen and flip, `DrawGameScreen`'s sweep, the unit blit and its shadows, the rasterisers, the order markers.
- [The GUI: surfaces, screens, gadgets and the palette](engine/ui.md) — The retained GUI, its leaves, popups and chat, screens and their lifecycle, gadgets, menus, the palette and the loading screen.
- [Input and the camera](engine/input.md) — The mouse object and thread, the cursor chain, clicks, the keyboard sampler, the camera and the viewport.
- [Multiplayer and the network](engine/network.md) — The network receivers, unit identity on the wire, the unit sync, match identity and the player metadata.
- [The COB engine](engine/cob.md) — The COB object, the loaded script, the runner and its opcodes, piece animation, get/set and the guarded runtime.
- [The demo recorder's engine boundaries](engine/recorder.md) — Where the recorder captures, the event sites, the puppet host, the takeover and the replay's engine-facing state.

**How it is kept.** A landing adds every engine address its code touched or merely read, with how each fact was established. A fact about a function that already has an entry extends that entry in place; a new section is for an area nobody has mapped yet. `tools/exemap-check.py` lists the code's new addresses that no page names, and warns when a new section names a function that already has an entry.

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
myself.] **Resolved 2026-09-11 against the pristine build** (*The per-player unit
cap* below): the two values are the `0x1F4` (500) immediates of `cmp eax, 0x1f4` at
`0x491658` and `mov eax, 0x1f4` at `0x491665` — the bound the ini value is compared
against, and the value stored when it exceeds the bound, which is exactly why the two
must be set identically. Their file offsets are **0x90A59 and 0x90A66** (`.text` is VA
`0x401000` at file `0x400`, delta `0x400C00`); the community's 0x90A50 and 0x90A60 are
the 16-byte hex-editor rows that hold them [INFERRED].

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
stock Steam install. **Retail 3.1 does read the key** — `0x491653` reads `UnitLimit`
with a default of 250 [VERIFIED, below] — but clamps it to **[20, 500]** before the
store, so `UnitLimit = 1500` yields 500, and 6553 is the width of the field, not a
value the retail engine will ever hold. The "6553" and the "v3.9.02 default is 1500"
are that patch's business, not retail's. (`AISearchMapEntries` is not examined here.)

### The recorder's patch sites — `tplayx.dll` 3.9.2.416 [DISASSEMBLED 2026-09-25]

TADR's demo recorder patches the engine as well, through its Delphi plugins. These are the sites of
the build players run (Escalation's `eplayx.dll`), each checked in the pristine exe and its immediate
found in that binary. What each feature does, and what to port, is in the
[merge exploration](tadr-merge-exploration.md#the-shipped-features-by-port-group). The recorder
writes some sites at a transient splice of the entry point `0x4E6FA0` (`push ebp; mov ebp,esp`), and
the rest at the game's first call into a DirectPlay export.

| VA | Stock code there | The recorder's patch |
|---|---|---|
| `0x491640`, `0x491659`, `0x491666` | the unit-cap default and ceiling (*The per-player unit cap* above) | 2-byte operand writes: 1500 |
| `0x447D87` | `jne` after `call 0x457B90` (the count of controller-2 players) in the battleroom's Multi handler | NOP 6: several AIs in one game |
| `0x45130F` | `push ecx; push "AI:%s"` and the `sprintf` that names an AI | a 16-character name buffer |
| `0x490DF9` | inside `SetGameSpeed 0x490DF0`, which clamps to [1, 20] (`cmp ebx,0x14`); the speed packet handler calls it too (`0x455952`) | clamp to the speed lock's range instead |
| `0x4965B3`, `0x496559` | the + and − keys' speed tests | the same clamp |
| `0x496099` | the in-game key handler's pause case | a jump created disabled and never written; tdraw's `LagSwitchGuard` hooks the same site |
| `0x480770` | COB `get` (`vt+0x44`) | a jump to the eight extra getters |
| `0x416BBB` | inside `+View` (`0x416B50`), before it writes `main+0x2A43` at `0x416BC1` | line-of-sight bookkeeping |
| `0x452B54`, `0x452B5E` | the two epilogues of the alliance setter | mark "alliances changed" |
| `0x46555F` | in the per-tick player loop `0x464F80` | after a change, run the full rebuild `Game_SetLOSState 0x4816A0` once |
| `0x481D63` (epilogues `0x481FAF`, `0x48207B`) | the sight removal `0x481D50` | repeat it for each ally |
| `0x482283` (epilogues `0x4824CF`, `0x482597`) | the true-LOS stamp `0x482270` | repeat it for each ally |
| `0x465AD4` | `UnitInPlayerLOS 0x465AC0`: its owner == player test | an allied owner answers "visible", before the cloak and submerged tests |
| `0x48BC3D` | the HotUnits cull `0x48BAE0` | NOP 0x13 bytes: the own-player shortcut |
| `0x4674AB`, `0x46750E`, `0x46782F` | the view player's radar scan `0x467440` | re-run loops 2–5 (`0x46750E..0x46782F`) per ally, writing each into `main+0x2A43` |
| `0x466E6F` | the minimap rebuild `0x466DC0` | draw allied units |
| `0x46AC85`, `0x46B013`, `0x46B10E` | the unit panel's owner tests | show allied units' details |
| `0x4D989B` | the crash writer: `push esi; call [0x4FC0EC]` (`CloseHandle`) | append a module list |
| `0x417B9B` | `call 0x4B7900`, the console command interpreter | a command table, empty in the shipped build |

### Where other patchers meet ours — the Patch Loader's hand-off and TADR's limit crack [DISASSEMBLED + MEASURED 2026-09-26]

What happens when these run beside Impure, and the suite that tests it, is
the wiki's Compatibility section, from [its overview](compat/overview.md). The engine facts:

- **`0x47BFA2`, `0x4B55FB`** — the exe's two `call 0x49F710`, and `0x49F710` is the
  `DirectDrawCreate` import thunk (`jmp [0x4FC02C]`). The Community Patch Loader's `patch_call`
  points both at the `DirectDrawCreate` of the module its `LoadLibraryA("tdraw.dll")` returned:
  without Impure that is TADR's, which calls whatever `ddraw.dll` it loaded; with Impure, which
  answers that request with itself (`tagpu_takeover.c`), it is Impure's own. Neither address is
  a site of Impure's table.
- **`0x4FC034`, `0x4FC038`, `0x4FC03C`** — the exe's `DPLAYX.dll` import slots, in the import
  address table in `.rdata` (read-only once bound): ordinal 2 `DirectPlayEnumerateA`, 1
  `DirectPlayCreate`, 4 `DirectPlayLobbyCreateA`, and nothing else; the descriptor is at RVA
  `0xFF194`, its name table at `0xFF218`. Reached only through the thunks `0x4FAFF0`
  (`jmp [0x4FC034]`), `0x4FAFF6` (`jmp [0x4FC03C]`) and `0x4FAFFC` (`jmp [0x4FC038]`), whose
  callers are `0x4CA435` (the enumeration), `0x4CA4D7` (the lobby object) and `0x4CA667`,
  `0x4CA922` (`DirectPlayCreate`). No other instruction reads the slots, and the exe holds no
  DirectPlay name to look one up by (DISASSEMBLED: the whole `objdump -d`, and `strings`). The
  first call is `DirectPlayLobbyCreateA`, at start-up, in single player too (MEASURED
  2026-09-26: the first export call every recorder build logs). A DirectPlay stand-in in the
  game folder — the 2006 recorder, or the Patch Loader whose exports forward to `tplayx.dll` —
  is reached through these slots alone, which is how Impure keeps every *game call* away from
  the recorder (`tagpu_takeover.c`, [GPU status](gpu-status.md) §2.6d). It does not keep the
  recorder from running: that is `0x4E6FA0` below, closed at the recorder's own entry point.
- **`0x4E6FA0`** — the exe's **entry point** (`objdump -f`: `start address 0x004e6fa0`), the C
  runtime start-up whose first bytes are `55 8B EC 6A FF` (`push ebp; mov ebp,esp; push -1`),
  ending in the call to `WinMain`. It runs after every statically linked DLL's `DllMain`, which
  is what makes it the place a DLL reaches when it wants the loader lock released.
  **TADR's recorder splices an `E9` jump over it from its own `DllMain`** and installs its code
  injections there — `SpliceInJump` on this address, then at the entry `UnSpliceJump` and
  `OnInitialize(OnMainRun=true)`, which skips the log and runs
  `Do_LoadTime_CodeInjections`/`InstallPlugins`, and on `DLL_PROCESS_DETACH`
  `UninstallCodeInjections` puts its sites back (`vendor/TADR/src/Recorder/
  InitCode_CoreExePatching.pas`, the unit `initialization`; `InitCode.pas`; [the recorder's
  page](deep-tadr.md)). The address is a constant in the recorder's own source, beside
  `0x004E0FD1` for the Boneyards exe. MEASURED 2026-09-27, `retail+tadr1` on Wine, reading the
  live process against the exe file: `0x4E6FA0` is back to stock by the time a game is up, and
  seven sites of the engine hold a jump or call into the recorder module — among them
  `0x417B9B` (where the stock `call 0x4B7900` reaches the chat-command interpreter),
  `0x447D87`, `0x45130F`, `0x480770`, `0x490DF9`, `0x496559` and `0x4965B3`. None is a site of
  Impure's fail-closed table, so §2.6b's net does not fire; the recorder also writes Impure's
  own value at the three unit-limit sites `0x491640`/`0x491659`/`0x491666`. With
  `tagpu_takeover.off` and the DirectPlay path open as well, the same read finds **24** sites.
  Impure closes this not at these sites but a step earlier, by making the recorder's own PE entry
  point inert before the loader calls it, so the splice is never written ([the
  takeover](compat/takeover.md) part 1); nothing here is a site of Impure's table either way.
- **`0x4E4708`, `0x4E71A0`, `0x4EADF2`** — three call sites of the same import thunk, stock
  `FF 15 00 C1 4F 00` (`call [0x4FC100]`), which the Community Patch Loader rewrites to
  `E8 rel32` followed by `90`: a direct call into the mod's own `win32.dll`, whichever build the
  mod ships (MEASURED 2026-09-27 out of the live process, Total Mayhem 11.3.0 and ProTA 4.8; the
  target differs with the DLL's load address). They are the mod's bytes and stay — Impure's
  reference-image check names them in the log and refuses only what leads into TADR
  ([the takeover](compat/takeover.md) part 4). Not sites of Impure's table.
- **`0x401064`** — NOP padding in the first code page. The loader writes `1` there as a variable
  (`patch_setbyte`), the handshake by which `tdraw.dll` knows the proxy is active.
- **`0x488C50`** — the unit-category name map's lookup-or-insert, `ret 4`, its argument a
  refcounted string passed by value. It binary-searches the sorted array
  `[0x51E6B4, 0x51E6B8)` of 8-byte `{name, mask}` entries with `_stricmp 0x4F8A70`, which is case-insensitive (it folds A–Z before comparing, `0x4F8A9A..0x4F8AB6`). On a name it does
  not hold it allocates the 0x40-byte mask (`push 0x40; call 0x4B4F10` at `0x488CC2`, the
  site Impure widens for 16 384 types), clears 16 dwords (`0x488CD2`), inserts through
  `0x488FB0` (`this` = `0x51E6B0`), then releases its two strings through `0x4C9390` and returns
  the mask. The carry flag is clear on both paths into `0x488CC2` (`je` after a `cmp` that
  found equality, `jne` after `test eax,eax`).
- **`0x42C028`** — one caller of `0x488C50`, in the unit-definition load: it reads the FBI key
  `wpri_badTargetCategory` (string `0x503E88`, default `"none"` at `0x503EA0`) and stores the
  mask at `def+0x231`. It runs on the Load Thread when a skirmish loads.
- **`0x4C9390`** — releases a refcounted string: the count is the dword before the text,
  decremented at `0x4C9396`, and the block is freed through `0x4E8820` when it reaches zero.
  `ecx` points at the string object.
- **The collision** — a pre-2026 TADR's `ModifyHook` at `0x488CC2` re-assembles the bytes it
  finds there as if they were stock. After Impure's `jmp` it leaves `E9 <stub> 90 90 90 90 90`
  through `0x488CCB` and a stub `push 0x800; jae 0x488D0D; …; jmp 0x488CCC` (read from the
  running process, Total Mayhem 11.3.0). The `jae` is always taken, the allocation is skipped,
  the frame is one dword low, and `0x4C9390` writes through `0x0042C02D`, the return address:
  *Access Violation at `0x004C9396`, illegal write to `0x0042C029`*, Load Thread. TADR's
  other `ModifyHook` sites fall on eleven more of Impure's unit-type relocations
  (`0x406DB5`…`0x406ED6`, `0x48BE08`, `0x48BF1E`).
- **The 3.9.02 exe's own bytes** — `0x406FB4` (an AI commander keeps its orders when hit),
  `0x43E784` (the reclaim cursor over any unit), `0x4266A5` (the DirectX version box),
  `0x4966E7` and `0x496776` (two debug keys), `0x4FCC7C` (the `+atm` amount),
  `0x501FD8` and `0x501FE4` (`+AI` and `+Control` from level 4 to 2), `0x5098A4`
  (`totala.ini` → `ta.ini`), and `0x40EAD6` (the path budget). The table is in
  [the setups page](compat/setups.md#the-3902-exe).

### TADR QoL configuration bytes and map owner markers

**SOURCE + DISASSEMBLED / MEASURED 2026-10-04.** Read for the
[supported native QoL audit](tadr-port/quality-of-life.md). These are foreign-patcher inputs,
not new Impure writes. Source claims are labelled separately from stock disassembly.

| Address / raw file offset | Meaning and evidence |
|---|---|
| `0x50390A/B` / `0x101F0A/B` | Default/max metal/geo snap radii, spare `.data` bytes read by pre-split TADR `tahook.cpp` (`v2024.3.31`, snap getter functions). M/P embedded loader INIs write `03 03`; native Z leaves its availability gate zero. Later E/T use compile-time defaults/maxima, E 0/0 and T 3/3. SOURCE + native loader/data measurement. |
| `0x503912/13` / `0x101F12/13` | Equivalent wreck snap radii. Native M/P loader writes `01 01`; Z's gate is zero. Later E/T configure 1/1. SOURCE + native loader/data measurement. |
| `0x50391F` / `0x101F1F` | Pre-split guarding-constructor behavior gate. ProTA's loader writes `01`; `v2025.8.29` `TABugFix.cpp` reads it before installing the hooks. Its stay/scatter dialog choices still default to Cavedog behavior. SOURCE + native loader measurement. |
| `0x483A75`, `0x483AD3`, `0x483B31` / `0x82E75`, `0x82ED3`, `0x82F31` | Immediate owner bytes of three stock `push 0x0A` instructions, whose boundaries are `0x483A74`, `0x483AD2`, `0x483B30`. The following calls at `0x483A7E`, `0x483ADE`, `0x483B38` all reach `0x423C50`. DISASSEMBLED in the pristine 3.1 image. ProTA's embedded loader changes all three immediates to `0B`; M lacks those writes and native E/Z/T retain `0A`. MEASURED in native fixtures. |
| `0x469A7B` → `0x469AAF` | TADR's `DrawPlayer11DTProc` hook site and LOS-skip continuation. It tests `(ecx & 0x0F) == 11`. SOURCE (`TABugFix.cpp`, inspected native release tags); this row does not claim a fresh disassembly of that draw block. Its constructor's three marker writes above are commented out. Thus the hook alone does not establish native map-DT visibility. |

Loader sections name **raw file offsets**. In these `.data` bytes VA = raw + `0x401A00`;
in the `.text` marker bytes VA = raw + `0x400C00`. The snap cells are `.data`, not `.rdata`.
Under takeover the mod's spare-byte values remain, but Impure does not yet consume these QoL
gates or reproduce the owner-11 LOS bypass.

The recorder's camera-sharing default was also checked in native DLL disassembly. The following
are **preferred-image DLL VAs**, not addresses in `TotalA.exe`:

| Native recorder | Instructions / data | Established fact |
|---|---|---|
| E/M/P/T 3.9.2.416 | `0x738334` pushes 1; `0x738336` loads string `0x7389FC` (`sharepos`); category string `0x7389DC`; `0x738343` calls options reader `0x71FD18`; `0x73834B` stores AL at object `+0x44` | Camera-sharing registry fallback is true. DISASSEMBLED + native strings. |
| Z 3.9.2.0 | `0x734089` pushes 1; `0x73408B` loads `0x7344FC` (`sharepos`); category string `0x7344DC`; `0x734098` calls `0x71D770`; `0x7340A0` stores AL at object `+0x44` | Same true fallback in the older recorder. DISASSEMBLED + native strings. |

Every native recorder contains `lockon`, `sharemappos` and replay-follow help. The old
`self_commands.inc` handler and registration establish `.lockon` as a local replay action with
no host-only gate; following requires camera data in the recording. These observations are
availability/default evidence, not a newly run native replay.

## Engine defects we patch — mapped by us [DISASSEMBLED + MEASURED 2026-09-23..25]

Seven places where the retail 3.1 image writes or reads memory it does not own (the fourth is the
composite scratch frame's writers, with the span tables of the rasterisers under them; the fifth
and sixth are a builder's build list and the download menus' records, which a large mod reaches;
the seventh is a weapon's ID, which the loader and two network receivers take unbounded), two
where it takes a player's payment and does not deliver (a feature reclaimed or destroyed while
the wreck pool is full, and a feature reclaimed twice through a cell that is not its anchor), one
where a network game can never start (two unit types with one unit-sync key), one where the peers
of a network game disagree (a weapon with the ID 253, 254 or 255 hitting a feature, which rides
with the seventh), two in a saved game's load (the features on the map's border are lost, because
the border mask runs before the restore; and a restored feature's state is written into a wreck
record it does not own when it did not come back as saved), the out-of-memory text, which
blames the disk, and four in combat, landing B1 of [the TADR port's section B](tadr-port/sim-fixes.md):
an explosion hits a unit found past its twentieth victim, or a feature past its sixty-fourth, once
for every cell of it in the blast; flak fired nearly straight up divides by zero; a unit whose
footprint ends on the map's last column or row is parked off the map, where nothing can hit it;
and a unit whose altitude is more than twice its distance from the north edge falls off the
line-of-sight grid, so no other player sees it; one more in combat, landing B2: an aircraft
that shares its cells with other aircraft can hold none of them, and then no explosion finds it;
three on the wire: five network receivers take a unit index, a type or a slot delta off the wire
with no bound (landing B3), a hit names its victim by a slot the owner reuses at once, so a hit in
flight lands on the next unit there (landing B4), and a peer still loading refuses the others'
first units, their commanders among them, until the round robin re-creates each (landing B5);
and five outside the battle, landing B6: each peer of a network game draws a wind of its own, a
yardmap string is parsed past its end, the saved-game loader's order fallback walks one record
past its table, the stockpile bar divides by a weapon's reload time without testing it, and a
range circle of radius 1 divides by zero.
`tagpu_patches.c` (`patch_engine_defects`, then `patch_loader_defects` for the last five)
patches all of them at every attach, in both builds: `ddraw.dll` is a static
import of the exe, so `DllMain` runs before the exe's entry point. Each patch is the identity on
every input the stock code handles correctly.

**Two classes** [DECIDED 2026-09-25, [How B fixes are held](tadr-port/sim-fixes.md#how-b-fixes-are-held)].
A **simulation** fix is one whose absence would let a player silently compute different shared
state, on an input stock does not fault on. Its sites are rows of the one fail-closed table the
raised limits use (`tagpu_limits_install`): the whole table is compared with the retail bytes
before any site is written, written as a whole or not at all, and a mismatch, a stub that cannot
be made, two rows over one byte, or a refused write ends the process through [the failure
report](tadr-port/raised-limits.md#the-failure-report), in the stock-limits build as in the
raised one. A **local** fix is one whose absence changes only a crash, a draw, a message or a
malformed input's fate: it compares and writes its own sites and is skipped, with its reason in
the `enginefix:` log lines, when its bytes differ from the retail exe, its stub cannot be
made, or its page cannot be made writable. Each, and why:

| fix | class | why |
|---|---|---|
| the sort buffer's end `0x469807` | local | the list feeds `DrawGameScreen`'s draw loops alone |
| the NULL plot `0x421E60` | local | without it the read through NULL faults |
| the terrain window `0x484057` | local | a draw, and a fault where stock reads off the tile map |
| the composite scratch | local | the frame is drawn state; stock writes past it |
| the out-of-memory text `0x49E700` | local | a message |
| flak's divides `0x49CF18`, `0x42F314`, `0x42F32E` | local | a player without it faults on the shot, which is not a silent divergence |
| the full wreck pool `0x423651` | simulation | without it a player keeps a feature the reclaim paid for |
| the reclaim's anchor mark `0x423892` | simulation | without it a group reclaim pays once per builder |
| the saved features on the border `0x43265A` | simulation | a loaded game silently lacks the border's features; retail has no saved multiplayer game, so the class follows from what the fix changes, the game's own state |
| a saved feature's record `0x4250C0`, `0x425185` | simulation | as the border fix |
| whole build lists `0x42DA58`, `0x42DAC7`, `0x42BEAF`, `0x42BEC3` | simulation | a builder's list is what the AI's pick `0x40BDB0` and the build menu offer |
| the download menus `0x42DCF0` | simulation | its records feed the builders' lists (`0x42BE30`) and the build menus |
| unique sync keys `0x42BD29` | simulation | a player without it keeps colliding keys, which the host's walk `0x46D9E3` answers with the first type that has them |
| weapon IDs `0x42E468`, `0x49D280`, `0x424575`, `0x455FB8` | simulation | the `0x0F` hit flag is a wire format; in the raised build these are the raise's own rows |
| one hit a victim an explosion `0x49A0A9`, `0x49A109`, `0x49A262`, `0x49A5CE` | simulation | who is damaged, and how much |
| the map's last column and row `0x47CC8B`, `0x47CCA3`, `0x47CCA9` | simulation | who can be hit |
| stacked aircraft `0x49A664`, `0x49A415`, `0x47CF98`, `0x4954ED` | simulation | who is damaged: an aircraft no in-rect slot names takes the splash it stood in |
| line of sight at the map's edge: `UnitInPlayerLOS`, `0x408090`, the AI probe, the order resolver `0x43F0E0`, the view player's map `0x467440`, the sight emitter `0x4825B0` (sixteen sites in 25 rows; the census is under "Line of sight at the map's edge") | simulation | what is acquired, the order resolved, what the AI probe keeps, what a unit sees |
| line of sight in local code: the cursor picker, the build cursor's site test, the feature helper, the radar rebuild's projectile dots, five particle leaves, positional sound (27 sites) | local | a cursor, a draw, a sound |
| the projectile pass's view `0x49BEE8` | local | a draw: whether the engine's frame shows and poses a projectile |
| wire robustness — the receivers' unbounded indices `0x4861F7` (`0x09`, with the sender's block), `0x4866E5`/`0x486753` (`0x0C` + killer), `0x489CED` (`0x0B`), `0x48B92B`/`0x48B960`/`0x48B985`/`0x48B9AD`/`0x48BA05`/`0x48BA5E`/`0x48B40E`/`0x48B49C`/`0x48BA9F` (`0x2C`, parsed from a zero-padded copy), the `0x2C`'s two nested unit references `0x44E0DE` (a move payload's target) and `0x48B574` (the round robin's carrier), `0x4553FE` (`0x0A` attach: child and parent), the `0x2C` length's capture in the receive `0x453595`/`0x45361F` (which also keeps the pump's message pointer on the buffer), and the splitter's short messages `0x463939`/`0x463B33` (landing B3) | local | every bound is stock-exact for a well-formed message and drops only a malformed or foreign one: a peer without the fix computes the same shared state (the same argument that makes the sort-buffer and terrain-window fixes local). The diverged `0x0D` drop rides the weapon-ID row `0x49D280` above: evidence §8 classes it simulation, only when diverged |
| stale hits — the incarnation on the wire and the two-tick hold: first-free `0x486036`, the free `0x486DC1`, `CreateFromNetwork`'s exit `0x48634F`, the array's reset `0x4854A0`, the sends `0x4560AE`, `0x489CB9`, `0x489CCD`, the dispatch slots `0x455F90`, `0x455FA0`, `0x455FA8` (landing B4; the subsection *Unit identity on the wire*) | simulation | which hits a unit takes and which slot a create gets; and it is a wire format: a peer without it sends bare `0x09`/`0x0B`s this build drops, and cannot read the tagged `0x05`s this build sends |
| ghost commander — the create refused during the load, held and replayed, a refused kill cancelling it, and the dirty create's position: the load's start `0x497F5E`, the frame function's call `0x49842F`, the dispatcher's refusal in state 5 `0x45477F`, the dirty entry's call `0x48BA00`, and the hold inside B4's `0x05` receiver (landing B5; the subsection *A create refused during the load*) | simulation | which units a peer holds at the start of play and where a dirty create puts one; a peer without it lacks the others' commanders until the round robin, as stock |
| one wind for every peer `0x490C5A`, `0x491903`, `0x4982CA`, `0x4C98FD` | simulation | projectiles and the fire spread move by the wind and wind generators produce by it, and each peer draws its own |
| yardmaps inside their string `0x42CF5E` | simulation | the yardmap decides where a unit can be placed, what it occupies and where others path |
| the saved-game order fallback `0x43A58D` | local | a malformed save's fate; this exe's writer always stores the order's name |
| the stockpile bar's divide `0x439D41` | local | a HUD draw |
| a range circle of radius 1 `0x438EDE` | local | a HUD draw |
| an order mode disarmed with nobody to order `0x49697B`, `0x499226` | local | the order byte `main+0x2CC3` and `BuildUnitID` are read only by the UI; no peer reads them |
| maps with no terrain `0x434D87`, `0x430B87` | local | which maps are offered; whether a network peer has the host's map is decided from the terrain file itself, not from this list |
| a level starts unpaused `0x49842A` | local | the pause bit is the local player's outside a network game, and a recording carries the ticks that ran; inside one the fix does nothing |

[Binary patches](binary-patches.html) §"Stock engine defects we patch" is the one-row-per-bug
register. The disassembly is `objdump -d -M intel` of `pristine/TotalA.exe.pristine`, and the
callers come from an E8/E9 rel32 scan of `.text`. The measurements are `tacli` instances on Two
Continents at 1024×768 with `scenarios/sort-row-overflow.json` (150 Peewees in one line, every
foot at the same world z), on Lava Run, Coast To Coast and Dark Side at 1920×1080 to 3840×2160,
on the camera branch's build for the terrain pass, on Town & Country at 1024×768 for the two
feature fixes, on Two Continents at 1024×768 for the composite scratch, with oversized units made
locally from stock models (never committed), on Core Prime Industrial Area with the unit types
`tools/unittypes_fixture.py` generates for the four large-mod fixes, on Two Continents with the
weapons `tools/weaponids_fixture.py` generates for the weapon IDs, on Two Continents with a save
and its load for the border features, and on Two Continents with the `scenarios/b1-*.json`
fixtures for the four combat fixes. Each compares against a build without the patch.


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

**Which run level the chat bar grants** `[DISASSEMBLED 2026-09-25; MEASURED the same day]`.
A chat line starting `+` (`0x493E04`) goes to `CallInternalCommandHandler 0x417B50(text,
level)` with **level 1, or 7 when SoftwareDebugMode bit `0x2` is set** (`main+0x37F2F`,
`0x493E14..0x493E20`; `tacli switches <i> cheats=on`), **OR 2 when `IsCheating 0x5091CC` is
non-zero** (`0x493E25..0x493E2F`). The dispatcher `0x4B7900` runs a matched entry's handler only
when `runLevel & level` is non-zero (`0x4B79CD..0x4B79D9`), and otherwise a fallback handler at
`0x51FC90` with its level at `0x51FC94` (set by `0x4B78E0`, registered at `0x4195DD` as
`0x417890`, level 4). The tables are registered at `0x4195C4..0x4195D8` (`0x501D38`, `0x501F48`,
`0x501FD0`, through `InitInternalCommand 0x4B7760`). Every entry of the `0x501FD0` table read
here — `PrintWeights`, `Profile`, `Reload`, `ReloadAIProfiles`, `Save` — is level 4, so it needs
the SoftwareDebugMode bit. **`Reload`** is `{0x5021A8 "Reload", 0x417490, 4}` at `0x5020C0`: with
an argument (`[argv+0xD0] > 1`) it resolves the unit name (`0x488B10`), calls `0x486E80(type)`
and then the one-type reload `0x42D1F0(type)` (*The level's unit-data load*, below).
`0x486E80` (one caller, `0x4174B8`) kills every unit of that type before the reload: it walks
the unit array (`main+0x14357..+0x1435B`, stride `0x118`) and calls `Send_UnitDeath
0x4864B0(unit, 8)` for each unit whose type index `[unit+0xA6]` equals the argument
(`0x486EA6..0x486EBF`); a type of 0 returns at once (`0x486E99`). So no live unit of the type
survives into the reload, and every unit that runs the reloaded COB is created after it.
`0x486ED0` right after it is the same walk with the test `[unit+0xA6] != 0` — every live unit
— and its one caller is the console command **`Kill`** (`{0x502244 "Kill", 0x4164B0, 4}` at
`0x501FE8`), which with no argument calls it and then `0x4904B0` with `ecx` = `[main+0x391ED]`, a method
on the object stored there (`0x4164BD..0x4164CD`). Measured: `+reload armllt` without the bit left the type's record as it
was (its ghost mask did not recompute); with it, `+reload ppllt` re-ran the FBI loader.
**MEASURED 2026-09-25**: `+reload armllt10` with two `ARMLLT10` towers, an `ARMSOLAR` and three
`CORSOLAR`s in play removed both towers and nothing else; without the `ARMSOLAR` the ARM side
had no unit left and the game went to `ENDMSN.GUI`.

## Mapped internal data structures

All from TADR's `tamem.h`, guarded by `static_assert` on field offsets and `sizeof` —
so machine-checked against the real layout rather than guessed. [VERIFIED]

| Structure | Facts established |
| --- | --- |
| `TAdynmemStruct` | The "god object". Reached via the global pointer-to-pointer at **`0x00511DE8`** (`*(TAdynmemStruct**)0x511DE8`). Enormous — mapped fields run past offset `0x142CB` (~82 KB). |
| `UnitStruct` | **`sizeof == 0x118`** (280 bytes). Pos x/z/y at 0x6A/0x6E/0x72; grid pos 0x76; footprint 0x7E/0x80; sort-bucket ptr 0x82 and link 0x8E (the carrier's chain instead while `+0x86` is set); `UnitType` (→`UnitDefStruct`) 0x92; owner 0x96; cloak state 0x10E; `UnitSelected`/state mask 0x110. |
| Unit array | `TAdynmemStruct::BeginUnitsArray_p` / `EndOfUnitsArray_p` (the latter at struct offset `0x1435B`) — a **flat contiguous array**, indexed by `short UnitInGameIndex`. |
| `PlayerStruct` | Inline at `TAdynmemStruct+0x1B63`, **stride `0x14B`** (331 bytes). TADR declares `Players[10]`, ending at `0x2851`; the engine builds and resets **eleven**, the eleventh at `0x2851` being what an index of 10 reads (*The unit and player records, byte by byte*). LOS buffer ptr at 0x7C, LOS tile w/h at 0x80/0x84. |
| Resource accounting | Inside `PlayerStruct`: `fCurrentEnergy`, `fEnergyProducton`, `fEnergyExpense`, `fCurrentMetal`, `fMetalProduction`, `fMetalExpense`, `fMaxEnergyStorage`, `fMaxMetalStorage` as **`float`**; lifetime totals (`fTotalEnergyProduced`, `fEnergyWasted`, …) as **`double`**. |
| `UnitDefStruct` (FBI) | **`sizeof == 0x249`** (585 bytes). CRCs at 0x13E/0x142/0x146; `buildLimit` 0x15A; **`weapon1/2/3` at 0x1EE/0x1F2/0x1F6**; `nMaxHP` 0x1FA; sight/radar/sonar 0x202/0x204/0x206; **`BuildAngle` 0x210** (the spawn's heading spread, §"`0x485A40`"). |
| Map / features | `FeatureStruct` is a **13-byte (`0x0D`) per-tile record**; `FeatureMapSizeX/Y` at `0x14233`/`0x14237`; `MAPPED_MEMORY_p` `0x14273`; `FeatureMap` `0x14287`. |
| Tile set / tile map | **`TILE_SET`** at `main+0x14283` → `{u32 count; u8* pixels}`: `count` 32×32 8bpp tiles of `0x400` bytes each, built by `LoadMap` and static for the map (Two Continents: 5062). **`TILE_MAP`** at `main+0x1428B`: `u16` tile index per 32-px cell, row stride `FeatureMapSizeX/2`. LoadMap allocates the tile map as `(pxW/32)·(pxH/32)` entries from `main+0x14223`/`+0x14227` (`0x48393C..0x483969`) and the tile set as `count·0x400 + 8` bytes (`0x483B53..0x483B80`) [DISASSEMBLED 2026-09-24]. The stock terrain pass checks its reads against neither; our bound is in "Engine defects we patch". Both byte-confirmed against the terrain blit `0x483FA0` ([terrain & depth](terrain-depth.html) §2) and read every frame by `tagpu_terr.c`. The Classic++ restorer reads them once more per map, on the render thread under the teardown fence, when the restore request is built (`tagpu_terr.c` `restore_publish`): every `TILE_SET` tile's edge texels for the tileability test; the whole `TILE_MAP` once to rank each tile by its distance in cells from the viewport (`restore_order`, the restore runs visible tiles first); and the whole `TILE_MAP` into a copy, every id bounded by the atlas's tile count, from which the neighbourhood keys are made (`nb_build`, [GPU status](gpu-status.html) §2.103). The pixels themselves are never copied again — the compute restorer reads the RGBA8 base atlas already on the GPU ([GPU status](gpu-status.html) §2.88). Read-only. |
| Live palette | `main+0x143A7`: 256 entries × 4 bytes, **R, G, B, pad** — and it does **not** cycle, and nothing moves it in play: its one writer is the PALETTE load `0x42A400`, run once per process (the writer survey is in "The palette the screen is presented with", below). [MEASURED 2026-09-05] Read out of the live
process with `tacli peek '*0x511DE8+0x143A7:x256'` ×4 for the whole table: **all 1024 bytes
identical across 16 samples over 8 s** in a live skirmish, on two maps, with a further 24
samples of entries 0..63 over 10 s. This row previously called it "the palette the engine
cycles for water", which was never measured and is wrong; the pixel half of the test is in
[terrain & depth](terrain-depth.html) §7. Read on the GAME thread only, by the frame packet's publisher (a 1 KB copy in every packet); `tagpu_pal_engine()` is that copy on the render thread, and **every world colour is built from it** since G20c: the base atlases the world passes and the restorer read, `uPal`, the effects' LHT flash table and the units' face-shade fit. The Gamma is applied once, to the finished world image ([GPU status](gpu-status.html) §2.3f). Read-only. |
| Grey remap table (fog of war) | Built by **`0x4BAD30`** into `*(0x51FBD0) + 0xCC`, 256 bytes — the same object whose `+0xC0` is the blend LUT we swap (see the blend-LUT section); `0x4B6220` is a two-instruction accessor, `mov eax,ds:0x51FBD0; ret`. For each of the 256 palette entries it averages R, G, B — literally `(R+G+B)/3`, compiled as the `0xAAAAAAAB` multiply-high then `shr edx,1` — writes that one value into all three channels of a scratch triple and calls `0x4BA9D0` for the index it stores. `0x4BA9D0` is a nearest-colour search `[INFERRED name]`: it prefilters candidates to a **±0x28 band on the R+G+B sum** and carries a `0x3B9ACA00` (1e9) best-distance sentinel. The builder is gated on `[obj+0xF1] & 1`, and it reads the palette at `main+0x143A7`. **[VERIFIED by disassembly 2026-09-04]**, `0x4BAD30`–`0x4BADE0` and `0x4BA9D0`–`0x4BAA80`; the rest of `0x4BA9D0` was not read. `TAGPU_GLSL_FOG_GREY_RGB` is the same `(R+G+B)/3` in full colour, kept and never snapped to a palette entry, and every world pass takes it in both presets; **no pass of ours reads this table** (G20d). Siblings building the other tables from that palette: `0x4BA750`, `0x4BADF0`, `0x4BABD0`, `0x4BAF30` — the table set is mapped in [shadows and cloak](shadows-cloak.html). |
| Spatial index | `SortGridBucket`, stride `0x0A`, list head at +0x06; buckets ptr at `0x1429F`, cols at `0x142A3`, plus a dedicated off-map bucket at `0x142B7`. |
| Projectiles | Count at `TAdynmemStruct+0x141F3`; **each projectile is `0x6B` (107) bytes**. |

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
  (`0x37EEC`), both **`unsigned short`** — which is where the widely quoted 6553 comes
  from, but retail never writes more than **500** into it: `0x491658` clamps the ini
  value to [20, 500] before the store (*The per-player unit cap* under *Documented
  patch offsets*). 250 is the retail *default*; 500 is the retail *ceiling*.
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
