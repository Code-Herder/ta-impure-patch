# Runtime Modification: Memory Patching, Injection & Hooking

## Summary

Runtime modification of `TotalA.exe` is **not greenfield — it is a mature, actively-maintained, open-source practice.** The community abandoned on-disk hex-editing years ago for in-process patching from a proxy DLL. Two repos carry the whole ecosystem:

- **`tanvanman/TADR`** ([repo](https://github.com/tanvanman/TADR)) — "TA Demo Recorder", source of `tdraw.dll` + `tplayx.dll`. ~600 files; last push **2026-08-31** (the day of this survey). Contains a bespoke x86 inline-hook framework, ~2000 lines of reverse-engineered structs, an engine-limit rewriter, and a cryptographic anti-cheat. [VERIFIED]
- **`FunkyFr3sh/Total-Annihilation-Patch-Loader`** ([repo](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader)) — MIT, C, explicitly "Made to replace the hex edited TotalA.exe in the Total Annihilation community patch". [VERIFIED]

Nobody uses `CreateProcess(CREATE_SUSPENDED)` + `WriteProcessMemory`. The vector is a **DLL search-order hijack via proxy DLLs** (`dplayx.dll`→`tplayx.dll`, `ddraw.dll`→`tdraw.dll`).

Two premises in the brief needed correction, and both matter:
- **The fixed image base holds** (`0x400000`, confirmed three ways) — *but* Windows Exploit Protection's Mandatory ASLR would break every absolute address in the ecosystem. It's a default, not a guarantee.
- **TA is not deterministic lockstep.** It is peer-to-peer state/event replication where each client owns and reports on its own units. That loosens the "bit-identical everywhere" constraint but substitutes a worse failure mode: divergence is silent.

## Trainers & Cheat Engine tables (what they reveal about memory layout)

The trainer scene is real but shallow, and **is not where the useful memory map lives**.

- **Cheat Engine table**, [FearLess Revolution t=105](https://fearlessrevolution.com/viewtopic.php?t=105). Author **Recifense**, 31-Dec-2015, CE 6.5, process `TotalA.exe`, "Game Version: 3.1.0.0 (steam)". Features: *Full Resources* (human player only), *God Mode* (human player's units only), "Some Pointers". 8.79 KiB, 1477 downloads. A second table by **mop** (Oct 2018, 2.64 MiB, GOG) adds *Instant build*, *Max Unit*. [VERIFIED — post read directly]
- **No addresses are published.** They live inside the attached `.CT` blobs, and the table is AOB/script-based (author: "The scripts use the CE command ASSERT and will not load if it is incompatible with the running game version"). **There is no public address list from the trainer scene** — that itself is a finding. [VERIFIED]
- **1990s scene**: MegaGames hosts a "Total Annihilation 3.1 IP trainer" by group **CLASS**, 26-Sep-1999 — *Instant Build, Perm LOS, Build Anywhere, Invulnerable, Cloak, Instant Kill, Instant Capture,* and notably ***Reject Other Players*** ([link](https://megagames.com/trainers/total-annihilation-31-ip-trainer-0)). Mechanism undocumented. [VERIFIED that it exists; method CLAIMED]
- [Cheat Happens](https://www.cheathappens.com/13915-PC-Total_Annihilation_cheats) has no trainer for original TA. [PLITCH](https://www.plitch.com/en/games/total-annihilation-2669) offers 3 (unlimited resources, godmode). [VERIFIED existence only]

**The real map is TADR, not any cheat table.** `src/DDraw/tamem.h` (1978 lines) defines `TAdynmemStruct`, `PlayerStruct`, `PlayerResourcesStruct`, `UnitStruct`, `UnitDefStruct`, `UnitOrdersStruct`, `WeaponStruct`, `ProjectileStruct`, `FeatureStruct`, `Object3doStruct`, `GUI*IDControl` and more, guarded by `static_assert(offsetof(...))` — e.g. `NumProjectiles == 0x141F3`, `MAPPED_MEMORY_p == 0x14273`, `SortGridBuckets_p == 0x1429F`. `PlayerResourcesStruct` is a plain float/double block (`fCurrentEnergy`, `fEnergyProducton`, `fCurrentMetal`, `fMaxMetalStorage`, `fEnergyWasted`, …). The Delphi side publishes globals in the clear in `src/Recorder/TAMem/TA_MemoryConstants.pas`: `TAdynmemStructPtr = $00511DE8`, `TAMovementClassArray = $00512358`, `TAunitsCategory = $0051E6B0`, `COBScriptHandler_Begin = $00512344`, `MultiplayerMapsList = $005122D4`. `TA_MemoryLocations.pas` exposes `LocalPlayerID`, `GameTime`, `GameSpeed`, `DevMode`, `MaxUnitLimit`, `UnitsPtr`, `ColorsPalette`, `Paused`, `AIDifficulty`. [VERIFIED]

## Launchers, loaders & injectors used with TA

**The Patch Loader is a `dplayx.dll` proxy.** `exports.def` declares `LIBRARY dplayx.dll` and forwards every DirectPlay export (`DirectPlayCreate`, `DirectPlayEnumerateA/W`, `DirectPlayLobbyCreateA/W`, `gdwDPlaySPRefCount`, `DllGetClassObject`) to `tplayx.dll`. TA imports DirectPlay, so Windows loads the local copy first and its `DllMain` runs before the game initialises. [VERIFIED]

`dllmain.c` then validates the build, applies a data-driven patch set, and chains the second DLL:

```c
patch_setbyte((void*)0x00401064, 1);   /* "Use byte in code cave as new variable to let
                                          tdraw.dll know that the dplayx.dll proxy is active" */
FARPROC dd_create = GetProcAddress(tdraw_dll, "DirectDrawCreate");
patch_call((void*)0x0047BFA2, (void*)dd_create);
patch_call((void*)0x004B55FB, (void*)dd_create);
```
[VERIFIED — verbatim]

**`tdraw.dll` is itself a `ddraw.dll` proxy**, AheadLib-generated: `src/DDraw/tdraw.def` exports the full DirectDraw surface as `AheadLib_*` forwarders with `DirectDrawCreate` implemented locally. It hooks `IDirectDrawSurface::Unlock` for a per-render-frame callback. [VERIFIED]

**TA Forever does not inject.** `ta-forever/gpgnet4ta` is a network shim ("tunnelling all game traffic through a single UDP port") that masquerades as local DirectPlay instances proxying remote peers. `apps/talauncher/talauncher.cpp` starts the game with plain `ShellExecuteEx` — no `CreateProcess`/suspend/`WriteProcessMemory` anywhere in the repo. TAF relies on proxy DLLs already in the game folder. [VERIFIED]

**Generic tooling — available but unused.** Detours is MIT and supports Win10/11; MinHook is a "Minimalistic x86/x64 API Hooking Library". Ultimate ASI Loader supports `ddraw.dll`, `dinput.dll`, `winmm.dll` proxy names — but **not `dplayx.dll`**. A code search across ThirteenAG's UAL and WidescreenFixesPack returns **zero** TA hits, and no TA project references Detours/MinHook/PolyHook. TADR rolled its own. [VERIFIED as an absence]

**Don't confuse API wrappers with code patching.** **cnc-ddraw** is a "re-implementation of the DirectDraw API" listing TA among supported games. **DxWnd** intercepts system calls (normal mode uses `SetWindowsHook`, optional "Use DLL injection" mode) and replaces **IAT import addresses**, not game code; it ships `build/exports/Total Annihilation.dxw`. Neither touches TA's instruction stream. [VERIFIED]

*Superseded:* TADR still ships `src/VisPatcher`, a Delphi "Visual Patcher 1.0" that edits the exe on disk. Its `src/TADemoSrc.txt` records the ancestral trick — "Patches totala.exe to load spank.dll instead of ddraw.dll". [VERIFIED]

## Practical hooking mechanics for a 1997 Win32 binary

**Fixed image base — verified three ways, not assumed.**
1. *Derived.* `res/patches.ini` annotates each patch with a file offset **and** a virtual address. Across 15 pairs the deltas fall into exactly three buckets — `0x400C00`, `0x401200`, `0x401A00` — i.e. three sections consistent with **ImageBase `0x400000`** (file `0x0009DDC0` → VA `0x0049E9C0`).
2. *Section map.* `patches.c` records `.text` file `0x400`→`0x00401000`, `.rdata` `0xFAE00`→`0x4FC000`, `.data` `0xFF600`→`0x501000` — an exact match for those deltas.
3. *Stated.* `src/DDraw/AreaDamageOverflow.cpp` comments **"ImageBase 0x400000, no ASLR"**. [all VERIFIED]

**Why it stays stable in 2026 — and what breaks it.** ASLR is opt-in via `IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE`, set by [`/DYNAMICBASE`](https://learn.microsoft.com/en-us/cpp/build/reference/dynamicbase-use-address-space-layout-randomization) and "first available in Windows Vista" — a 1997 binary cannot have it. The [PE spec](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) further notes the default EXE ImageBase is `0x00400000`, that "the default behavior of the linker is to strip base relocations from executable (EXE) files", and that `IMAGE_FILE_RELOCS_STRIPPED` means the file "must therefore be loaded at its preferred base address". [VERIFIED]

**The caveat:** Exploit Protection's **Mandatory ASLR** "forcibly relocates images *not* compiled with /DYNAMICBASE", and its "Do not allow stripped images" sub-option blocks reloc-stripped binaries "*instead of allowing them to load at their preferred base address*" ([reference](https://learn.microsoft.com/en-us/defender-endpoint/exploit-protection-reference), [customize](https://learn.microsoft.com/en-us/defender-endpoint/customize-exploit-protection)). Enabled, it breaks every absolute address in the ecosystem — or stops TA loading. It is off by default [CLAIMED — not in the MS tables; check `Get-ProcessMitigation -System` for `ForceRelocateImages : NOTSET`]. **A new project should use `GetModuleHandle(NULL) + RVA`; it costs nothing and removes the failure mode.**

**DEP** is on by default and "only configurable for 32-bit (x86) apps" — but it isn't why `VirtualProtect` is needed: `.text` is mapped `MEM_READ|MEM_EXECUTE` without `MEM_WRITE`, so code writes need it regardless. DEP only bites on *new* trampoline memory — allocate it executable. [VERIFIED]

**Patch primitives.** `patch.h` is the minimal complete toolkit; each helper wraps the write in `VirtualProtect(..., PAGE_EXECUTE_READWRITE, ...)` and restores: `patch_call` (`E8` rel32, returns original target), `patch_ljmp` (`E9`), `patch_sjmp` (`EB` rel8), `patch_call_nop`, `patch_setbyte/word/dword`, `patch_setbytes`, `patch_clear`. [VERIFIED]

**Main-loop hook.** `src/DDraw/GameTickHook.cpp` installs a 5-byte inline JMP at **`0x4969cb`** and reads state from the global pointer:
```c
static unsigned int GameTickHookAddr = 0x4969cb;
TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
int gameTime = taPtr->GameTime;
m_hooks.push_back(std::make_shared<InlineSingleHook>(GameTickHookAddr, 5, INLINE_5BYTESLAGGERJMP, GameTickHookProc));
```
`LagSwitchGuard.h` documents a second: "DeltaTime hook at 0x4967f8 (after CALL ApplyDeltaTime in GameTickFunction)". [VERIFIED]

**The framework** (`src/DDraw/hook/`) provides `SingleHook`, `InlineSingleHook`, `ModifyHook`. Modes: `INLINE_5BYTESLAGGERCALL/JMP` (0x1001/2 — save/restore full register context *and* replay displaced instructions), `INLINE_UNPROTECTEVINMENT` (0x1003), `INLINE_5BYTESNOREDIECTCALL/JMP` (0x1004/5 — context saved, displaced bytes *not* replayed), `INLINE_MODIFYCODE` (0x1006), `INLINE_SINGLEJMP` (0x1007). Routers receive a `PInlineX86StackBuffer` exposing saved `Eax…Edi`, `EFlags` and the return address; returning sentinel `X86STRACKBUFFERCHANGE` (`0x7798FFAA`) signals the router rewrote the return address. Comments are bilingual Chinese/English. [VERIFIED]

**Defensive patching.** TADR verifies expected bytes before writing, e.g. `TABugFix.cpp`:
```c
const DWORD AntiNukeTargetSearchAddr = 0x0049D120u;
const BYTE  AntiNukeTargetSearchExpected[5] = { 0x8B, 0x44, 0x24, 0x08, 0x53 };
```
[VERIFIED]

**Growing limits without a code cave.** `EngineLimits.cpp` documents stock ceilings — `STOCK_PROJECTILE_LIMIT = 300`, `STOCK_EXPLOSION_LIMIT = 300`, `STOCK_MODEL_EFFECT_LIMIT = 100`, `STOCK_MODEL_EFFECT_POOL_BYTES = 0x186A0`, `PROJECTILE_SIZE = 0x6B` — then allocates replacement pools as DLL statics and rewrites the absolute addresses and bound constants embedded in TA's instruction stream to point at them. The DLL's own image supplies the memory, so no cave is needed. Siblings do the same for weapon types, unit types, SFX, composite buffers and AI search-map entries. The only explicit **code cave** found is the single flag byte at `0x00401064` (PE-header padding). [VERIFIED]

## Existing TA-specific runtime mods

All source-available:

| Project | What it is |
|---|---|
| `tdraw.dll` (TADR `src/DDraw`) | The big one — bugfixes, megamap, hotkeys, unicode, limit raises, HUD, anti-cheat |
| `tplayx.dll` (TADR `src/Recorder`, Delphi) | DirectPlay proxy + demo recorder |
| [Patch Loader](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader) (`dplayx.dll`) | Loads/patches; replaces the hex-edited exe (MIT) |
| [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) | `ddraw.dll` wrapper for modern display (API-level) |
| [DxWnd](https://github.com/DxWnd/DxWnd.reloaded) | Generic API hooker; ships a TA profile |
| [gpgnet4ta](https://github.com/ta-forever/gpgnet4ta) / TAF | Network shim + launcher, no injection |

`tdraw.dll` ships in **per-mod build configurations** — `tdraw-prota/escalation/tazero/ota/mayhem/bta.zip` — each enabling a different feature subset at compile time, renamed per mod (`taesc.dll`, `mdraw.dll`, `zdraw.dll`). Provenance per `src/DDraw/tdraw.txt`: **SY_Yeha** ("TA Hook", whiteboard, hotkeys) → **Xpoy** (megamap, unicode, weapon-ID crack) → **Rime** → current maintainers (Axle, FunkyFresh, tagROCK, TAG_Venom). [VERIFIED]

Separately, [`ioma8/totala-re`](https://github.com/ioma8/totala-re) is a small radare2 static-RE effort (entry0 `0x004e6fa0`, main `0x0049eda0`, core `fcn.0049e830`, 391 functions) aimed at a Rust rewrite, not runtime patching. [VERIFIED]

## Anti-tamper, integrity checks & version matching

**Stock TA has no check on its own binary.** No checksum-of-self, no anti-debug, no packing — which is why plain `VirtualProtect` + write works with zero evasion. The `PLAYER_INFO_20` lobby packet carries only `versionMajor`/`versionMinor`; no exe hash on the wire. [VERIFIED for packet contents; CLAIMED as an absence for the self-check]

**Stock TA does have a per-unit CRC handshake — "unit sync".** The one integrity mechanism Cavedog shipped; it covers game *data*, not code. Sub-packet **`0x1A UNIT_DATA`** (14 bytes, "Unitsyncdata" in 1990s RE notes) carries `{sub, id, crc}`: sub-2 = unit CRC, sub-3 = restrict-list status/limit, sub-0 = clear (`libs/tapacket/TPacket.h` `TUnitData`; `src/Docs/lobbyprot.txt`). Engine-side, `tamem.h` documents `UnitDefStruct::CRC_FBI` at `0x13E` as "the unit's identity on the wire (and the unitSyncMap key)" and `CRC_all` at `0x142` as "the script/COB/download CRC, XOR-folded with CRC_weapons, computed lazily by `UnitInfo_CalcScriptCRC` (0x0042a610)". TA's lobby surfaces results: *"You have CRC errors on N units!"*, *"You are missing N of M units!"* [VERIFIED]

**But it degrades rather than refuses** — mismatches grey out Restrict List entries or stop commanders spawning rather than blocking the game ([Steam thread](https://steamcommunity.com/app/298030/discussions/0/487877107149654252/)). [CLAIMED, consistent with the verified protocol]

**Stock MP version bytes exist.** `res/patches.ini`: `; Change version # in multiplayer battleroom (all players must match) - 0009DDC0 (0049E9C0) / 0009DDC9 (0049E9C9)`, exposed as `MultiplayerVersionMajor/Minor`. `dllmain.c` reads the same bytes. [VERIFIED they exist; engine enforcement CLAIMED]

**The loader gate-keeps the build** with a byte signature at a module-relative offset — the one place relative addressing appears:
```c
if (!game_exe || memcmp((char*)game_exe + 0x00010000,
        "\x14\x68\x78\x1B\x50\x00\x8D\x4C\x24\x1B", 10) != 0)  /* "Game version not supported..." */
```
[VERIFIED]

**The community built its own anti-cheat, aimed squarely at these techniques.** `ChallengeResponse.{h,cpp}` implements an HMAC-SHA256 nonce challenge/response inside hijacked 65-byte TA chat packets (`ChatHijackId::ChallengeResponse` = `0x2b`). Commands: `ChallengeRequest`, `TDrawVersionRequest`, `Gp3VersionRequest`, `TPlayVersionRequest`, `ExeVersionRequest`, `AllVersionRequest`; replies `ChallengeHashReplyModules`/`ChallengeHashReplyGameData`. `GetModulePaths()` enumerates loaded modules via `EnumProcessModules`, keeps those in the game directory, and hashes their **on-disk** files; `HashUnits/Weapons/Features/GamingState/MapSnapshot` cover game data. The anti-cheat code is scattered across randomised segments via `#pragma code_seg(push, ".text$" RANDOM_CODE_SEG_n)` so it has no stable address. A mismatch produces a **HUD warning, not a kick** — enforcement is social. [VERIFIED]

**TAF hashes the install at two layers.** `libs/talaunch/LaunchServer.cpp` `generateGameFileHashes` SHA-256s the exe, every `*.dll` and `*.gp3`, and units inside `*.ufo` HPI archives; `gameFileVersionMismatch` aborts launch. `apps/gpgnet4ta/GpgNetGameLauncher.cpp` `verifyGameFileVersions()` CRC32-whitelists server-side: *"crc32 version mismatch … not whitelisted for competitive play … or play an unranked game instead."* TAF also derives `modHash` = MD5 over `(unitId + unitCRC)` from observed `0x1A` traffic (`UnitDataRepo::hash()`). **So a modified `TotalA.exe` on disk is detectable; a pristine exe patched at runtime by a matching `tdraw.dll` is not — but the DLL is hashed too.** Note the escape hatch: unranked play explicitly permits non-whitelisted files. [VERIFIED]

## The multiplayer determinism constraint

**This inverts the premise of the question: TA is *not* deterministic lockstep.** [VERIFIED]

TA is peer-to-peer DirectPlay with **per-owner state and event replication** — each client is authoritative for its own units and broadcasts what they *did*, not just the orders given. The sub-packet vocabulary in `libs/tapacket/TPacket.h` is unambiguous: `UNIT_TAKE_DAMAGE_0B`, `UNIT_KILLED_0C`, `WEAPON_FIRED_0D`, `AREA_OF_EFFECT_0E`, `UNIT_START_SCRIPT_10`, `UNIT_BUILD_FINISHED_12`, `PLAY_SOUND_13`, and the variable-length position/stat stream `UNIT_STAT_AND_MOVE_2C`. Unit IDs are partitioned per player (`unitId % maxUnits == 1` identifies *that sender's* commander) and death is announced by the owner (`onUnitDied(sourceDplayId, unitId)`, `GameMonitor2.cpp`). Corroborating: `.tad` is a **raw packet log**, not an order log — `struct Packet { uint16 time; uint8 sender; bytestring data; }` (`TADemoRecords.h`); a lockstep engine would record only inputs. TADR's own notes say positions "are transmitted regularly so it would probably stabilize after a min or so". [VERIFIED]

**What this means for binary modding:**
- **No bit-exact simulation requirement.** A client computing damage differently won't desync-and-halt; it broadcasts different events and the game drifts into *wrong* play. For a modder this is worse, not better: **failures are silent and gradual, not loud and immediate.**
- Because each peer owns its units, a modified client can assert things about *its own* units that others accept. That is why a 1999 trainer could ship *Invulnerable*/*Instant Build*/*Instant Kill* as multiplayer-relevant options, and why the community had to build an anti-cheat rather than rely on sync-checking.
- **Render-side** changes (colours, fonts, megamap, HUD, reload bars) are genuinely free and need no coordination — TADR exposes many as per-user `.ini` settings. **Simulation-side** changes must still ship identically to everyone, not to preserve determinism but to keep every peer's world-model consistent.

TADR's `AlliedBuildQueueSync` and `GridClaimTieBreak` (tie-break for simultaneous grid claims) exist to paper over conflicting independent conclusions — the bug class this architecture produces. `LagSwitchGuard` addresses the exploit it invites.

## What exists vs. what would be greenfield

**Already exists (do not rebuild):** the injection vector, patch primitives, an x86 inline-hook framework with register-context routers, a main-loop hook point, ~2000 lines of struct layouts, an engine-limit rewriter, a cryptographic anti-cheat, and a version-matching pipeline. All MIT or source-available.

**Genuinely greenfield:**
- **No published address list from the trainer/CE scene** — `.CT` files are AOB blobs. The authoritative map (`tamem.h`, `TA_MemoryConstants.pas`) is code, not documentation.
- **No modern hooking library in use** — nobody has ported TA modding to MinHook/PolyHook/Detours or Ultimate ASI Loader. TADR's framework is bespoke, undocumented outside its headers, partly commented in Chinese.
- **No standalone injector** — everything is load-time via proxy DLL; there is no way to hot-load into a running game.
- **No plugin API** — `tdraw.dll` is a monolith; adding a feature means forking a large C++ codebase and shipping a new CRC'd build to every player.
- **No consolidated symbol/offset database** — no IDA/Ghidra DB, no `.json`/`.h` export. `ioma8/totala-re` is the only public static-RE effort and is early-stage.
- **No ASLR-robust addressing** — every project hardcodes absolute VAs. Rebasing on `GetModuleHandle(NULL) + RVA` is trivial and nobody has done it.

## Sources

- [tanvanman/TADR](https://github.com/tanvanman/TADR) — `src/DDraw/{tdraw.txt,tamem.h,tafunctions.h,hook/hook.h,GameTickHook.cpp,EngineLimits.cpp,TABugFix.cpp,ChallengeResponse.{h,cpp},LagSwitchGuard.h,AreaDamageOverflow.cpp,tdraw.def}`, `src/Recorder/TAMem/{TA_MemoryConstants,TA_MemoryLocations}.pas`, `src/Docs/{lobbyprot.txt,newfeatures.txt}`, `src/TADemoSrc.txt`, `src/VisPatcher/main.pas`
- [FunkyFr3sh/Total-Annihilation-Patch-Loader](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader) — `dllmain.c`, `patch.h`, `patches.c`, `exports.def`, `res/patches.ini` (MIT)
- [ta-forever/gpgnet4ta](https://github.com/ta-forever/gpgnet4ta) — `apps/talauncher/talauncher.cpp`, `libs/talaunch/LaunchServer.cpp`, `libs/tapacket/{TPacket.h,TADemoRecords.h,UnitDataRepo.cpp}`, `apps/gpgnet4ta/{GameMonitor2,GpgNetGameLauncher}.cpp`, `apps/replayer/TaReplayer.cpp`
- [ioma8/totala-re](https://github.com/ioma8/totala-re) — `README.md`, `docs/DISASSEMBLY_NOTES.md`
- Microsoft: [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) · [/DYNAMICBASE](https://learn.microsoft.com/en-us/cpp/build/reference/dynamicbase-use-address-space-layout-randomization) · [Exploit protection reference](https://learn.microsoft.com/en-us/defender-endpoint/exploit-protection-reference) · [Customize exploit protection](https://learn.microsoft.com/en-us/defender-endpoint/customize-exploit-protection)
- [Detours](https://github.com/microsoft/Detours) · [MinHook](https://github.com/TsudaKageyu/minhook) · [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) · [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) · [DxWnd.reloaded](https://github.com/DxWnd/DxWnd.reloaded)
- [FearLess Revolution t=105](https://fearlessrevolution.com/viewtopic.php?t=105) · [MegaGames CLASS trainer](https://megagames.com/trainers/total-annihilation-31-ip-trainer-0) · [Cheat Happens](https://www.cheathappens.com/13915-PC-Total_Annihilation_cheats) · [PLITCH](https://www.plitch.com/en/games/total-annihilation-2669)
- [Steam: unit-sync mismatch symptoms](https://steamcommunity.com/app/298030/discussions/0/487877107149654252/) · [Steam: 3.9/3.1 network compatibility](https://steamcommunity.com/app/298030/discussions/0/487877107145540935/) [CLAIMED] · [tauniverse "TA Hook" thread](https://www.tauniverse.com/forum/showthread.php?t=12347) · [taforever.com/mod_standards](https://www.taforever.com/mod_standards) (editorial only — **no** technical version-matching detail despite the title)

**Caveats on rigour.** tauniverse is Cloudflare-blocked to fetch tools (HTTP 403), so community claims come from Steam instead. There is no official Cavedog statement of the netcode model, but that conclusion is drawn from the decoded wire protocol and demo format in source, not assertion. **No memory address, offset, or struct field here was invented** — every one is quoted from a linked source file. The ImageBase `0x400000` figure was derived arithmetically from 15 offset/VA pairs in `res/patches.ini`, then cross-checked against the section map in `patches.c` and an explicit comment in TADR.
