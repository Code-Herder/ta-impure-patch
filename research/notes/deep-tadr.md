# TADR / tdraw.dll — the TA Community Patch core

Source read: `github.com/tanvanman/TADR`, master @ `13d71dd` (2026-08-31), full history
deepened locally (568 commits, 2023-09-29 → 2026-08-31). All `file:line` citations are
relative to the repo root. `[VERIFIED]` = read in the code; `[CLAIMED]` = asserted by
in-repo prose I did not verify against a binary.

## Summary

TADR is three binaries from one repo: **`tdraw.dll`** (C++, `src/DDraw/`) — the TA
Community Patch proper; **`tplayx.dll`** (Delphi, `src/Recorder/`) — the demo recorder;
**`Server.exe`** (Delphi, `src/Server/`) — the replayer. MIT, with the copyright chain
spelled out in `LICENSE`: *2003 SJ, Yeha → 2019 Xpoy → 2023 Axle1975, FunkyFr3sh → 2026
Axle1975, tagROCK* (recorder line: *2003 SJ, Yeha → 2015 Rime*). [VERIFIED]

Neither DLL patches `TotalA.exe` on disk beyond one string. Everything else is done at
runtime, in-process, against a hard-coded TA 3.1c image base — ~809 distinct
`0x004xxxxx`/`0x005xxxxx` addresses appear across `src/DDraw/`. [VERIFIED]

## Bootstrap: how the DLL takes control

The load vector is an **import-name hex edit**, and `src/VisPatcher/main.pas:99-165` is
the historical proof of the technique: it refuses any `TotalA.exe` whose size ≠ 1178624
bytes ("requires TA 3.1c"), scans the image for the literal ASCII `ddraw.dll`, and
overwrites the first **five** characters in place (`main.pas:155-156`). That is why every
shim in the ecosystem has a five-letter stem: `ddraw` → `spank` (historic), `tdraw`,
`taesc`, `mdraw`, `zdraw` — `src/DDraw/tdraw.txt:56-62` lists the per-mod DLL/ini names.
The same trick handles DirectPlay: `dplayx` → `tplayx`. `src/DDraw/ta entry point.txt:9`
records the import-name string at `0x4FDB98`, and `ChallengeResponse.cpp:489` reads the
live one straight out of the image (`std::string DPlayXOutermostWrapper((const char*)0x4ff9e4);`)
to identify whichever wrapper is outermost. [VERIFIED]

`tdraw.def` re-exports all 22 DirectDraw entry points as `AheadLib_*` forwarders, except
`DirectDrawCreate=DirectDrawCreate @8`, which is ours. So control arrives three times:

1. **`DllMain`/`DLL_PROCESS_ATTACH`** (`ddraw.cpp:169-266`). Loads `dplayx.dll` first
   (the patch-loader rewrites the registry path, so it must precede config reads), then
   `LoadLibrary("ddraw.dll")` for the real one, sets up three shared memory-mapped
   sections, then installs ~35 feature modules in a flat list —
   `EngineLimits::Install()`, `StartPositions`, `AutoTeam`, `UnitDefExtensions`,
   `VeterancyHack`, `NotToAir`, `SurfaceFire`, `ShareGuard`, … each `#if`-gated by
   `config.h`.
2. **`DirectDrawCreate`** (`ddraw.cpp:311-334`) forwards to the real DDraw and wraps the
   returned `IDirectDraw` in `class IDDraw`, which wraps every surface in `IDDrawSurface`
   — that is where blitting hooks and `CTAHook` (`iddrawsurface.cpp:175`) hang.
3. **Inline hooks into `TotalA.exe`'s code**, installed from module constructors.

`IDDraw::IDDraw` (`iddraw.cpp:24-30`) sanity-checks the exe: bytes at `0x4AD494` must be
`00 55 E8`, else `CompatibleVersion = false`. [VERIFIED]

The **Delphi** side takes a different route because `DllMain` runs under the loader lock.
`src/Recorder/InitCode_CoreExePatching.pas:1-30` documents the constraint verbatim, then
splices a 5-byte `E9` jump over the **exe entry point at `0x004E6FA0`** (`:96-101`). The
thunk is two-stage (`:70-88`): a naked asm shim calls a Pascal `InitThunk_Stage2`, which
first `UnSpliceJump`s itself out and then runs `DoInitialize` — so the patch is transient
and TA's real entry point runs immediately after. `SpliceInJump`
(`src/Recorder/PluginEngine.pas:220-243`) is `VirtualProtect(PAGE_READWRITE)` →
`ReadProcessMemory` (backup) → `WriteProcessMemory(E9 + rel32)` → restore protection.
[VERIFIED]

Note: `ddraw.cpp:87` declares `AddtionInitHook` and `:129` deletes it, but nothing ever
constructs it — dead scaffolding from before `DllMain` did the work. [VERIFIED]

## The inline-hook framework (with code)

`src/DDraw/hook/` is Xpoy-era Chinese-commented code (an English translation was added
inline) implementing three classes over one primitive.

**The primitive** — `MemWriteWithBackup` (`hook/etc.cpp:300-325`): SEH-wrapped
`VirtualProtect(PAGE_EXECUTE_READWRITE)` → `memcpy` → restore original protection. **No
expected-bytes check**; validation is the caller's job (only `EngineLimits` does it).
**Modes** (`hook/hook.h:82-90`): `INLINE_UNPROTECTEVINMENT` (raw byte overwrite),
`INLINE_SINGLEJMP`, `INLINE_5BYTESLAGGER{CALL,JMP}` (register-preserving trampoline),
`INLINE_5BYTESNOREDIECT{CALL,JMP}`, `INLINE_MODIFYCODE`.

**`InlineSingleHook`** is the one feature authors use. `InitHookClass`
(`hook/InlineHook.cpp:271-408`) heap-allocates a stub of
`X86INLINEROUTERENDOFF + Len*4 + 9` bytes, `VirtualProtect`s it RWX, and `memcpy`s a
**template** of the hand-written naked `X86InlineHookRouter` (`InlineHook.cpp:15-137`)
into it. The template `pushad`/`pushfd`s, resolves a **per-thread**
`InlineX86StackBuffer` (`hook.h:60-78`) from a linked list keyed on
`GetCurrentThreadId()` (`:139-191`), guards re-entrancy with a `0xbd88` sentinel, copies
the saved registers into the buffer, calls the router, restores. Three fixed offsets in
the copy are then patched: `…STACKBUFFEROFF (0x0b)` = the buffer pointer (`:347-353`),
`…STRACKADD4 (0x0)` NOP-filled for JMP mode (`:356-366`), `…CALLADDROFF (0x58)` = rel32
to the router (`:368-370`). Displaced instructions are relocated after the stub by
`X86RedirectOpcodeToNewBase` (`etc.cpp:180`, over an instruction-length decoder
`GetOpCodeSize` at `etc.cpp:462` and relative-operand fixer `X86ShallToRedirect` at
`etc.cpp:95`), followed by `E9` back to `AddrToHook + Len` (`:394-396`). `Hook()`
(`:410-430`) NOP-fills `Len` bytes at the site and writes the 5-byte `E9`. So:
**yes — `VirtualProtect` + 5-byte `E9` + a relocating trampoline.** [VERIFIED]

**Control return** is by contract on the router's return value. Returning `0` resumes
normally; returning `X86STRACKBUFFERCHANGE` (`0x7798FFAA`, `hook.h:94`) tells the stub to
honour a `rtnAddr_Pvoid` the router overwrote. Registers are always restored from the
struct, so the router can also *edit* them.

**The pattern to copy** is `GameTickHook.cpp` in full (56 lines):

```cpp
static unsigned int GameTickHookAddr = 0x4969cb;                    // :26
static unsigned int GameTickHookProc(PInlineX86StackBuffer X86StrackBuffer)
{
    TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
    int gameTime = taPtr->GameTime;
    for (auto& f : GameTickHook::GetInstance()->getCallbacks()) f(gameTime);
    return 0;                                                        // :36
}
GameTickHook::GameTickHook() {
    m_hooks.push_back(std::make_shared<InlineSingleHook>(
        GameTickHookAddr, 5, INLINE_5BYTESLAGGERJMP, GameTickHookProc));  // :41
}
```

`NotToAir::CheckRouter` (`NotToAir.cpp:58-71`) shows the redirect form: read `pBuf->Eax`
/ `pBuf->Ebx`, set `pBuf->rtnAddr_Pvoid = kRejectAddr`, `return X86STRACKBUFFERCHANGE`.
Modules own hooks in `std::vector<std::unique_ptr<SingleHook>>` and RAII-unhook on
destruction. Convention is a singleton with a static `Install()` called from `DllMain`,
plus a header comment stating the hook address, the exact displaced opcode bytes, their
length, and which register holds what — e.g. `WeaponTdfHook.h:12-17`. [VERIFIED]

## tamem.h: the community memory map

`src/DDraw/tamem.h` — 1978 lines, `#pragma pack(1)`, ~60 struct/typedef definitions
(preceded by 68 forward declarations at `:9-68`) and 14 enums. It is a **layout** map, not
an address map: only 10 literal addresses appear in it. Named engine structures include
`TAdynmemStruct` (`:400-661`, the ~0x392xx-byte god object reached through
`*(TAdynmemStruct**)0x00511DE8`), `PlayerStruct` (`:116-172`), `UnitStruct` (`:986`,
stride `0x118`), `UnitDefStruct` (`:863-976`, the FBI record, `0x249` bytes),
`WeaponStruct` (`:174-195`, stride `0x115`), `ProjectileStruct`, `FeatureStruct`,
`FeatureDefStruct`, `UnitOrdersStruct`, `SortGridBucket`, `_COBHandle`, `Object3doStruct`,
the ten `_GUI*IDControl` variants; enums `WeaponTypeMask`, `UNITINFOMASK_0/1`, `LOSTYPE`,
`CURSORINDEX`, `SharedStates`. Offsets **are** `static_assert`-checked — 40 assertions,
36 of them `offsetof`, in two blocks (`:972-976`, `:1940-1973`):

```cpp
/* 0x13E */ unsigned long CRC_FBI;      // tamem.h:874
/* 0x142 */ unsigned long CRC_all;
/* 0x146 */ unsigned long CRC_weapons;
/* 0x14A */ short FootX;
...
static_assert(offsetof(UnitDefStruct, CRC_FBI) == 0x13E, "UnitDefStruct::CRC_FBI moved");
static_assert(sizeof(UnitDefStruct)            == 0x249, "UnitDefStruct size changed"); // :976
static_assert(sizeof(UnitStruct)               == 0x118, "UnitStruct stride");          // :1944
static_assert(offsetof(TAdynmemStruct, NumProjectiles) == 0x141F3, "PROJECTILES_ARRAY_Count"); // :1963
```

Comments carry reverse-engineering provenance (IDA/Ghidra names and corrections — e.g.
"IDA/Ghidra wrongly splits as dw+field_1E dw" at `:127`, the `cloakcost` float-vs-long bug
at `:918-921`). `EngineLimits.cpp:516-530` adds a second assertion block pinning
`sizeof(ProjectileStruct)==0x6B`, `ExplosionStruct==0x54`, `DebrisStruct==0x34`,
`sizeof(void*)==4`. Delphi keeps a parallel map in `src/Recorder/TAMem/`
(`TA_MemoryStructures.pas` 1614 lines, `TA_MemUnits.pas` 1905,
`TA_MemoryLocations.pas` 679). [VERIFIED]

## Byte-patching layer & validation

**`src/DDraw/HardCodedValue.cpp` is dead** — the entire 245-line file is inside one
`/* … */` (`:1` and `:245`), it `#include`s a `WeaponIDLimit.h` that does not exist, and
it is absent from `ddraw.vcxproj`. It preserves the historic weapon-ID/SFX patch tables.
`HardCodeFunctions.cpp` (862 lines) *is* compiled: it holds replacement C functions plus
the global address table (`AddtionInitAddr=0x0049E909` `:808`,
`AddtionInitAfterDDrawAddr=0x049E9A0` `:809`, `MPUnitLimitAddr=0x0044CAFE` `:832`,
`UnitLimit0/1/2Addr=0x491640/0x491659/0x491666` `:833-835`,
`AISearchMapEntriesLimit=(LPDWORD)0x0040EAD6` `:777`, `Sfx_mallocBufSizeAddr=0x00471C87`
`:772`). [VERIFIED]

The **modern** patch layer is `EngineLimits.cpp`. `struct Patch { DWORD address;
std::vector<BYTE> expected, replacement; const char* name; }` (`:151-157`).
`ValidatePatches` (`:219-234`) requires `expected.size()==replacement.size()` and an
SEH-guarded `memcmp` against live memory for **every** patch before any write.
`ApplyPatch` (`:236-247`) is `MemWriteWithBackup` + `FlushInstructionCache`. Install is
all-or-nothing with rollback (`:534-554`), refuses to run if TA already allocated its
projectile pool (`:508-514`), and on failure `AbortIfInstallFailed` (`:572-585`) puts up
a `MB_SYSTEMMODAL` box and `ExitProcess`es — a half-patched client would desync.
Significant patches: projectile allocation size/clear-count (`:277-280`), ten projectile
creation caps (`:282-288`), a stack-probe trampoline replacing `SUB ESP,0x4C0`
(`:290-311`), the explosion pool relocated out of `TAdynmem` into a DLL-side array via
seven `mov`-for-`lea` rewrites (`:314-398`), model-effect base/end operands ×13
(`:435-449`), two wholesale allocator replacements for aux effects (`:452-501`).
The older `LimitCrack.cpp` path does **not** validate — it writes DWORDs blind.

Delphi has its own layer: `TPluginData.MakeReplacement / MakeRelativeJmp /
MakeStaticCall / MakeNOPReplacement` (`PluginEngine.pas:104-127`) with backup/restore in
`TCodeInjection` (`:32-49`); ~40 plugins registered in `Plugins.pas:57-103`
(`UnitLimit`, `WeaponsExpand`, `UnitInfoExpand`, `MaxScriptSlots`, `OrdersOverride`,
`COB_extensions`, `SpeedHack`, `LOS_*`, …). No expected-bytes check there either — the
TODO at `PluginEngine.pas:4-7` admits it. [VERIFIED]

## Engine limits raised (resolved table)

The authoritative statement of stock values is the shipped `src/DDraw/totala.ini`, which
annotates every key with "TA v3.1 default" and "TA patch default".

| Limit | Stock | TADR | Where |
|---|---|---|---|
| Units per player | **250** (`totala.ini:31`) | **1500**, ini `UnitLimit` | 4 DWORD writes |
| Pathfinding cycles | 1333 | 66650, ini `AISearchMapEntries` | `0x0040EAD6` |
| SFX vectors | 400 | 20480 (`totala.ini:47`); malloc = ×10 | 20 sites + 1 hook |
| Unit-type IDs | 512 | 16000, ini `UnitType` | 17 patches |
| Composite (model) buffer | 600×600 | 1280×1280 | `0x458195` |
| Projectiles | 300 | **3000** | `EngineLimits.h:5` |
| Explosions | 300 | 3000 | `EngineLimits.h:6` |
| Model-effect slots | **100** | **1000** | `EngineLimits.h:7` |
| Aux (debris) effects | 300 | 3000 | `EngineLimits.h:8` |
| Weapon IDs | 256 | 4096 heap overflow, **off by default** | `WeaponIdOverflow.h:29-30` |

**Resolving the unit-limit conflict.** Neither of your notes is right, and the "6553"
figure appears nowhere in the repo. `src/Recorder/plugins/UnitLimit.pas:37-67` quotes the
actual TA 3.1 disassembly at the disputed address:

```
.text:0049163A mov  eax, TAdynmemStructPtr
.text:0049163F push 5DCh                    ; = 1500, the ReadIniFileValue default
.text:00491644 push offset aUnitlimit       ; "UnitLimit"
.text:00491653 call ReadIniFileValue
.text:00491658 cmp  eax, 5DCh               ; upper clamp
.text:00491665 mov  eax, 5DCh
.text:00491678 cmp  eax, 14h                ; lower clamp = 20
.text:0049167D mov  eax, 14h
```

So **stock TA 3.1 already reads `UnitLimit` from the ini and clamps it to [20, 1500]**;
the `[20,1500]` clamp at `0x49163A` in your notes is *vanilla behaviour, not a patch*.
1500 is the ceiling, 250 is the value TA ships in its ini. What TADR does is rewrite the
three `5DCh` immediates — `0x491640` (default), `0x491659` (compare), `0x491666`
(clamp-to) — plus a fourth multiplayer site at `0x44CAFE`
(`LimitCrack.cpp:466-473`, addresses at `HardCodeFunctions.cpp:832-835`). The Delphi
plugin writes 2 bytes per site (`UnitLimit.pas:93-104`), the C++ writes 4. The "500 →
1500 or 5000" line is a **stale comment** at `UnitLimit.pas:3` that the code below it
contradicts. One live inconsistency: `LimitCrack.cpp:43` uses a hard-coded fallback of
**3663** if the ini key is missing, while the shipped ini says 1500 and its own comment
says the range is 20–1500. Same pattern at `:41` — code fallback 16000, shipped ini
20480. [VERIFIED]

**Weapon IDs 256→16000 is not current.** `tdraw.txt:8` explicitly says the weapon-ID
crack is "not present in current release"; the ini keys `WeaponType` and
`MultiGameWeapon` are documented in `totala.ini:64-76` but read nowhere in the code. The
live replacement is `WeaponIdOverflow` — a heap array for IDs ≥ 256 up to 4096
(`WeaponIdOverflow.h:29-30`) plus a `WeaponFiredExt` packet, both behind
`TDRAW_EXTENDED_WEAPON_IDS`, which `config.h:69` defaults to **0**. [VERIFIED]

## New data-driven extension points for modders

This is the most transferable idea in the repo, and the trick is that **TADR does not
write a parser**. TA's own TDF/FBI reader is an object with generic accessors, and their
addresses are known:

```cpp
typedef int   (__thiscall* TdfFile_GetInt_t)   (TaTdfFile*, const char* key, int def);
typedef double(__thiscall* TdfFile_GetFloat_t) (TaTdfFile*, const char* key, double def);
typedef void  (__thiscall* TdfFile_GetString_t)(TaTdfFile*, char* buf, const char* key,
                                                size_t len, const char* def);
static TdfFile_GetInt_t    TdfFile_GetInt    = (TdfFile_GetInt_t)   0x4C46C0;
static TdfFile_GetFloat_t  TdfFile_GetFloat  = (TdfFile_GetFloat_t) 0x4c4760;
static TdfFile_GetString_t TdfFile_GetString = (TdfFile_GetString_t)0x4C48c0;
// UnitDefExtensions.cpp:183-189
```

The mechanism is: **hook the loader mid-parse, while the parsed file object is still
alive in a register, and call the engine's own getter with a key the engine never asks
for.**

*Unit defs* — `UnitDefExtensions` hooks `0x42bf97` inside the FBI loader
(`UnitDefExtensions.cpp:6,16`). Its router reads `ECX` = `TaTdfFile*` and `EBP` =
`UnitDefStruct*` (`:193-194`), walks a registry of keys other modules registered, calls
the right getter per key (`:196-225`), and stores results in DLL-side vectors indexed by
`UnitDefStruct::UnitTypeID`. `registerIntKey/FloatKey/StringKey` return a handle whose top
two bits tag the type (`(INT_KEY << 30) | index`, `:41`), so the getters reject a
type-mismatched handle (`:80,97,114`). Registration happens in `DllMain` before any FBI is
read (`CUnitRotate::RegisterUnitDefKeys()`, `CBuildGhost::RegisterUnitDefKeys()`,
`TransportedExplosions.cpp:239-240`).

*Veterancy* is the worked example. `VeterancyHack` registers a **string** unit-def key
`VeterancyThresholds` (default `"5 10 15 20 25"`) plus `VeterancyAccuracyBuffRate`, then
installs eight routers — capture cost `0x4043d8`, kill-counter draw `0x46b306` /
`0x467ccf`, take/deal damage, aim/accuracy/reload buffs (`VeterancyHack.cpp:39-255`).
Each reads the string for the unit's `UnitTypeID`, memoises `splitInts` of it (`:295-311`),
computes a level from `unit->Kills`, then rewrites a register or substitutes a `"Vet%d"`
string into the HUD path. Pure FBI-driven; no engine structure grown.

*Weapons* — `WeaponTdfHook` (`WeaponTdfHook.cpp`) hooks `0x42E4AB` in `LoadWeaponTdf`
(6 displaced bytes, `EBX` = `TdfFile*`, `EBP` = `WeaponTypedef*`) and fans out to
registered handlers; `Context::getInt` is a one-liner over `0x4C46C0` (`:10-13`).
Two consumers:
- **`nottoair`** (`NotToAir.cpp:35-49`) claims a real engine bit — `WTM_NotToAir`, bit 31
  of `WeaponTypeMask` at def offset `0x111` — so the combat-path test is just
  `pBuf->Eax & WTM_NotToAir` with zero lookup (`:60`).
- **`surfacefire` / `nottounderwater` / `notoverwater` / `notoverland`**
  (`WeaponTags.h:19-25`) have no spare bits left, so `WeaponTags` keeps a side table:
  a flat `uint32_t[256]` indexed by `(def - &Weapons[0]) / 0x115` with a single unsigned
  compare covering both bounds (`WeaponTags.h:58-70`), falling back to an
  `unordered_map` for overflow weapons.

Two design rules are worth stealing outright. First, **lazy hook installation**:
`WeaponTags::OnFirstUse(tag, fn)` (`WeaponTags.cpp:139-146`) defers installing a hot-path
hook until a loaded TDF actually uses the key — "Vanilla OTA uses none of these, and every
one of them patches a combat hot path" (`WeaponTags.h:32-33`). Second, **assign, don't OR**,
the tag word per load, which removes the need for a wipe hook (`WeaponTags.cpp:112-118`).
[VERIFIED]

## Case study: share guard design

`src/DDraw/ESCALATION_SHARE_GUARD_DESIGN.md` (209 lines) designs one anti-abuse feature
end to end and is the best available template for "how to add a rule to a 1997 engine".
Its shape:

1. **Problem, then the cheapest design.** Two rules — delay bulk structure shares (10 per
   30 s, then the *whole* batch waits 30 s), and refuse `.take` when the target's
   commander is destroyed. It opens by rejecting a fancier alternative ("creator-tag unit
   estate") because **neither rule needs replicated state** (`:6-9`) — the decisive
   criterion in a non-lockstep peer-to-peer game.
2. **Explain the engine bug the feature closes.** `:38-49`: a dropped player whose
   commander died is *never eliminated*, because only a unit's owner may declare it dead
   and their client is gone; remote clients clamp `Health` to 0 and leave the alive flag
   set. That is what makes the base takeable.
3. **Turn the bug into a detector with no heuristics.** `Health <= 0 && alive` is
   uniquely reachable in that state (`:56-60`); commander identity comes from TA's own
   category bitmask `FindSpot_CategorysAry("Commander")` (`0x00488c50`), chosen over name
   comparison because that misidentifies captured commanders (`:62-65`).
4. **Find the single choke point.** `_ShowText` (`0x00463E50`) is where *every* route to
   `.take` converges — typed chat, TA's timeout dialog, tdraw's own vote button (`:82-87`)
   — and it is send-only, so a hook there never sees received chat (`:88-90`).
5. **Record hook mechanics and traps.** 10-byte prologue of whole instructions;
   `[Esp+4]`/`[Esp+8]` are the args because the prologue has not run; suppress via a naked
   `RET 0x10` stub and **not** the epilogue, which would `ADD ESP,0xC8` and unbalance the
   stack (`:97-100`). Plus a Ghidra signature correction (`:102-103`) and a note that the
   `UNITS_GiveUnit` address "allows only one router, so it is extended rather than
   re-hooked" (`:122-123`).
6. **State the accepted tradeoffs and the residual blind spot** (`:105-107`, `:207-209`).
7. **A falsifiable test plan.** Test 0 is `grep -q "transfer will complete in" tdraw.dll`
   to prove the feature is even compiled in — "A whole session was lost on 2026-07-31 to a
   `Public\` build that was silently `TDRAW_CONFIG_PROTA`" (`:163-164`). Each row carries
   an "if it fails, suspect X" column, and the doc names which test discriminates (`:178`).
8. **An open-questions list** naming the one unverified assumption — whether `GameTime` is
   ticks or milliseconds — and the test that would catch it (`:202-205`).

[VERIFIED — read in full]

## Per-mod configuration

One source tree, six DLLs. `config.h:6-23` requires exactly one `TDRAW_CONFIG_*` macro
(a `#pragma message` warns and defaults to PROTA, and an implicit-default build turns on
the profiler and dev dumps), then includes one of `config_prota.h`, `config_escalation.h`,
`config_ota.h`, `config_tazero.h`, `config_bta.h`, `config_mayhem.h` (`:25-37`). Each
defines a `TDRAW_CONFIG_NAME` string (logged at `ddraw.cpp:174`) and ~30 feature flags:
`SHARE_ABUSE_GUARD`, `TAKE_CLAIM_ENABLE`, `LAG_SWITCH_GUARD_ENABLE`,
`REPAIR_RATE_FIX_ENABLE`, `WEATHER_REPORT{,_WIND,_TIDAL}`, `MEGAMAP_FEATURES`,
`DEFAULT/MAX_MEX_SNAP_RADIUS`, `AREA_DAMAGE_OVERFLOW_ENABLE`, … The OTA↔ProTA diff is
purely these values. Flags are consumed both at `#if` sites around `Install()` calls
(`ddraw.cpp:236-266`) and inside modules, so a disabled feature compiles to nothing.
`config.h` also enforces coherence at build time — `#error` if a config forgets to define
`REPAIR_RATE_FIX_{REPAIR,SELFHEAL}_MULTIPLIER` (`:117-122`), and `#error` if either is
≠ 1 while the fix is off (`:130-133`). CI builds all six in a matrix and ships
`tdraw-<config>.zip` with `totala.ini`, `tdraw.txt` and `LICENSE`
(`.github/workflows/compile.yml:52-92`), after running `tools/check_win7_imports.py`
against the artifact to reject any post-Win7 import. [VERIFIED]

## Anticheat

`ChallengeResponse.cpp` (1550 lines) + `HmacSha256.cpp`. TA is **not** lockstep — it is
peer-to-peer with authoritative per-owner unit updates — so this cannot be a state-hash
comparison. It is instead a **challenge-response attestation of what each client loaded**,
carried in-band over a hijacked chat packet.

Transport: TA's `CHAT_05` subpacket is 65 bytes; TADR treats a chat whose first text byte
is NUL as an extension packet and dispatches on the byte at +2 (`ChatHijackIds.h:3-13`).
`ChallengeResponse` owns id `0x2b`. `ChallengeResponseMessage` is `static_assert`ed to
exactly 65 bytes with the last byte held at zero, so the recorder's packet sizer does not
over-read (`ChallengeResponse.h:38-59`); recorders round-trip these as opaque chat.

Protocol: a client broadcasts `ChallengeRequest` with a 32-byte nonce; peers reply with
`ChallengeHashReplyModules` (32) and `ChallengeHashReplyGameData` (33)
(`ChallengeResponse.h:27-35`). The HMAC key is **not** the nonce alone — it interleaves
the nonce's eight DWORDs with eight `RANDOM_CODE_SEG_*` build constants
(`ChallengeResponse.cpp:541-551`). Those are 64 random DWORDs in
`random_code_seg_keys.h`, regenerated by `random_code_seg_keys.ps1`, and simultaneously
used as `#pragma code_seg(".text$<random>")` section names around every anticheat
function (`:599-601`) — the key material *is* the code layout, so a tamperer cannot
precompute a valid reply without the exact build.

Two digests. **Modules**: `TotalA.exe` and every DLL in the game directory, hashed **from
their on-disk bytes** (`SnapshotModules`, `:470-505`), with the outermost `dplayx`
wrapper identified by reading the exe's own import string at `0x4ff9e4`. Hashing disk
rather than memory is what lets it survive TADR's own runtime patching. **Game data**:
weapon damage/AOE/`WeaponTypeMask`, feature footprints and burn/damage/energy, unit defs,
gaming state, map snapshot (`:756-800`). Verification is `memcmp` per peer, and the result
is **advisory** — mismatches are counted and surfaced as chat/log warnings ("fails
dll/exe verification!", `:692-701`), not enforced by disconnect. Complementary runtime
guards: `LagSwitchGuard`, `VoteReject`. [VERIFIED]

## Lineage & contributors

`README.md`: "Forked from https://svn.riouxsvn.com/tadr with contributors: Rime, Xpoy,
N72, Fnordia, SJ and Yeha." `tdraw.txt:6-11` gives the fuller narrative: **SY_Yeha**
(whiteboard markers, the TA Hook, new hotkeys) → **Xpoy** (hotkeys, shortcut commands,
Unicode fonts, megamap, weapon-ID crack, GOG music patch) → **Rime** → present, "with
contributions from Axle, FunkyFresh, tagROCK and TAG_Venom", crediting TAG_Venom, Gamma
and Wotan for testing. `LICENSE` dates the chain: 2003 (SJ, Yeha) → 2015 (Rime, recorder)
→ 2019 (Xpoy) → 2023 (Axle1975, FunkyFr3sh) → 2026 (Axle1975, tagROCK); the replayer line
runs 2003 Fnordia → 2015 Chaos. Git authorship over 568 commits (2023-09-29 → 2026-08-31):
Axle1975 467, tanvanman 26, FunkyFr3sh 25, tagROCK 16, RdKL 14, TAG_Venom 12, plus four
others. The Chinese comments throughout `src/DDraw/hook/` are Xpoy-era and carry
inline English translations. [VERIFIED]

## How you would add a new feature (practical recipe)

1. Reserve a config flag in every `config_*.h`, defaulted off via `#ifndef` in
   `config.h`; add a `#if`-guarded `Install()`/`Shutdown()` pair to `ddraw.cpp`'s
   `DllMain` lists.
2. Add or reuse a struct in `tamem.h`; pin every offset you rely on with
   `static_assert(offsetof(...))`.
3. Write `MyFeature.h` whose header comment names each hook address, the exact displaced
   bytes and their length, and the register→meaning mapping. Prefer 5–6 byte,
   position-independent displaced instructions.
4. Implement a singleton owning `std::unique_ptr<InlineSingleHook>` members constructed
   as `(addr, len, INLINE_5BYTESLAGGERJMP, Router)`.
5. In the router, read/write `pBuf->Eax…Edi`; to redirect, set `pBuf->rtnAddr_Pvoid` and
   `return X86STRACKBUFFERCHANGE`, else `return 0`. Pick a redirect target that cannot
   loop back into another of your hooks (`SurfaceFire.cpp:41-46` documents a real freeze
   caused by exactly that).
6. For a new FBI/TDF key: `UnitDefExtensions::registerIntKey/FloatKey/StringKey` at
   attach time, or `WeaponTags::RegisterKey` + `OnFirstUse` for weapons. Never write a
   parser — call the engine's `0x4C46C0`/`0x4c4760`/`0x4C48c0`.
7. For a raw byte patch, copy `EngineLimits`' `Patch{address, expected, replacement,
   name}` discipline: validate all, apply all, roll back on any failure, `ExitProcess`
   rather than run half-patched in multiplayer.
8. Crossing the wire: reserve an id in `ChatHijackIds.h` first; keep the packet at 65
   bytes with byte 64 zero.
9. Write the design doc first, in the shape of `ESCALATION_SHARE_GUARD_DESIGN.md`.

## Sources

All paths relative to `github.com/tanvanman/TADR` @ `13d71dd`. Hooks:
`src/DDraw/hook/{hook.h,InlineHook.cpp,Hook.cpp,ModifyHook.cpp,etc.cpp}`,
`GameTickHook.cpp`, `tahook.cpp`, `NotToAir.cpp`, `SurfaceFire.cpp`. Memory map:
`src/DDraw/tamem.h`, `src/Recorder/TAMem/*.pas`. Bootstrap: `ddraw.cpp`, `iddraw.cpp`,
`tdraw.def`, `ta entry point.txt`, `src/VisPatcher/main.pas`,
`src/Recorder/{InitCode_CoreExePatching.pas,InitCode.pas,PluginEngine.pas}`. Limits:
`EngineLimits.{h,cpp}`, `LimitCrack.cpp`, `UnitTypeLimit.h`, `IncreaseSfxLimit.h`,
`HardCodeFunctions.cpp:772-835`, `HardCodedValue.cpp` (dead), `WeaponIdOverflow.h`,
`totala.ini`, `src/Recorder/plugins/UnitLimit.pas`. Extension points:
`UnitDefExtensions.{h,cpp}`, `WeaponTdfHook.{h,cpp}`, `WeaponTags.{h,cpp}`,
`VeterancyHack.{h,cpp}`. Design: `ESCALATION_SHARE_GUARD_DESIGN.md`. Config: `config.h`,
`config_*.h`, `.github/workflows/compile.yml`, `tools/check_win7_imports.py`. Anticheat:
`ChallengeResponse.{h,cpp}`, `HmacSha256.{h,cpp}`, `ChatHijackIds.h`,
`random_code_seg_keys.{h,ps1}`. Lineage: `README.md`, `LICENSE`, `tdraw.txt`,
`git shortlog`.
