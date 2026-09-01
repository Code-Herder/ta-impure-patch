# Cavedog's Plugin ABI & the Historical Patch Corpus

*Survey date 2026-08-31. Scope: the shipped Cavedog `TotalA.exe` only.*

**Method note.** `files.tauniverse.com` is an open Apache autoindex and is the single best
entry point to the historical corpus — the whole 1999–2012 patcher archive is at
`/files/ta/old-stuff-for-v3.1-only/unofficial-patches/`. I downloaded the small patchers there
and did static analysis only (`sha256sum`, `unzip -l`, `strings`, `objdump`, `xxd`, `cmp`).
Nothing was executed. Crucially I also pulled Cavedog's **official 3.1c patch**
(`official-files/official-patches/manual-install/ta1x-31c.zip`), which yields a genuine stock
`TotalA.exe` — **1,178,624 bytes, md5 `f76d367fbbd43008ae04c0f314a61b13`,
sha256 `367bcdc2d25bb48a2599ee6e2e09ca28f73accbe66aa5a7b7d9cc8a5088db7c3`, dated 1998-07-30**.
Every offset below was then checked byte-for-byte against that baseline. `www.tauniverse.com`
(the forum) returns 403 to non-browser clients and was not accessed.

## Summary

Two headline results.

1. **The `TotalAExt` plugin ABI does not exist.** The premise that TA shipped a documented
   plugin mechanism keyed on DLLs exporting `TotalAExtVersion`/`TotalAExtAction` is
   **refuted** on four independent lines of evidence (below). `jtaext` does *not* use such an
   API. **[VERIFIED — negative]**
2. **But Cavedog did leave one real, name-based, `GetProcAddress`-driven optional-DLL hook in
   the shipped binary**: `DebugHelper.dll` → `DebugFunc1`, gated behind the `-debughelper`
   switch. That, plus the registrable in-game command table, is the whole of the engine's
   sanctioned extension surface. **[VERIFIED — from disassembly]**

Alongside that, the historical patch corpus turned out to be recoverable in full, with source,
and **every published offset in it verifies exactly against stock 3.1c.**

## (A) The official TotalAExt plugin API

**It isn't there.**

- **Not in the binary.** `strings | grep -i totalaext` returns nothing across seven distinct
  `TotalA.exe` builds (stock 3.1c, the 3.9.02 community-patch exe, TA:Escalation Gold, Total
  Mayhem 1.13 and 8.15, the Multicore build, the TADR drop-in). A `LoadLibrary`/`GetProcAddress`
  ABI must carry its symbol names as literal strings; they are absent.
- **Not in `jtaext`.** `jtaext.dll` (J. Rennison, file-dated **2006-02-03**, contact
  `jonathan_rennison@hotmail.com`) exports exactly **one** symbol, `FactoryClickPatch`, at
  **ordinal 1**. `jtaext_full.zip` ships the complete NASM source, and `patch.asm` shows the
  mechanism verbatim — an 86-byte hand-written stub written over the exe at `ORG 0x41ABBB`:
  `push <"jtaext.dll">` → `call [0x4FC0D4]` (LoadLibraryA) → `push 1; push eax;
  call [0x4FC094]` (GetProcAddress **by ordinal 1**) → `jmp eax`; on failure
  `push -1; call [0x4FC100]` (ExitProcess), which is exactly the "TA terminates with error code
  -1" the README describes. It is a bespoke inline hook, not a sanctioned API.
- **Not in the community corpus.** Xon's `ddraw.dll`/`Dplayx.dll`, the four Xpoy-era
  `ddraw.dll` weapon-ID builds, and TA Hook's `hook.dll` export only DirectDraw/DirectPlay
  entry points and `_InstallHooks`/`_KillHooks`. No `TotalAExt*`.
- **Not on GitHub.** Authenticated code search: `TotalAExtAction` → **0 results**,
  `TotalAExtVersion` → **0 results**. Control query `InitInternalCommand` → 32 results
  including three `tanvanman/TADR` files, so the index does cover this ecosystem.

**What *does* exist — the DebugHelper hook.** At **VA `0x004CBB70`** (file `0xCAF70`) the engine
contains a complete optional-plugin loader, recovered by disassembly:

```
push  0x50B530            ; "DebugHelper.dll"
call  [0x4FC0D4]          ; LoadLibraryA     -> else "Couldn't find library."  (0x50B4F0)
push  0x50B524            ; "DebugFunc1"
push  esi
call  [0x4FC094]          ; GetProcAddress   -> else "Couldn't find function." (0x50B508)
mov   ecx,[esp+8] / push ecx / call eax      ; DebugFunc1(arg) — one pointer argument
push  esi / call [0x4FC0C0]                  ; FreeLibrary immediately after
ret   0x4                                    ; __stdcall, 1 arg, returns DebugFunc1's value
```

A second call site sits at `0x004DA157`; the `-debughelper` command-line switch is matched at
`0x004DA1EE` behind a run-once flag byte at `0x005289CC`. Errors go to `OutputDebugStringA`
(`[0x4FC18C]`). So the contract is: **one exported `DebugFunc1`, `__stdcall`-called with a
single pointer, library freed straight afterwards.** It is a developer assert/diagnostics hook,
not a gameplay extension point — no callbacks in, no engine handle out, no versioning. The exe
also dynamically resolves `reporter.dll` (`0x00507AF8`), `online.dll` (`0x004FD4D0`),
`psapi.dll`, `IMAGEHLP.DLL` and `dsetup.dll`, but these are crash-reporting, Boneyards and
DirectX-setup plumbing rather than mod hooks.

No Cavedog SDK, plugin documentation, or extension header was found. TAU's
`official-files/official-documents/` holds only the manual, the English PDF and a pre-release
FAQ — no developer material.

## The in-engine command table & dev switches

This *is* a genuine registration ABI, and I resolved it completely — including the built-in
table base addresses and entry count, which were previously unpublished.

`InitInternalCommand` at **`0x004B7760`** takes one pointer to a NULL-name-terminated array of
12-byte `{const char* name; void(__stdcall*)(char* argv[]); DWORD runLevel;}` entries
(`NORMAL=1`, `CHEATING=2`, `DEBUG=4`). The engine registers **three** such tables from one init
function, at `0x004195A7`, `0x004195C9` and `0x004195D3`:

| Table | VA range | Entries | Contents |
| --- | --- | --- | --- |
| NORMAL (1) | `0x00501D38`–`0x00501F3C` | 43 | `+noshake +contour +scrollspeed +iface +give +cdplay +cdstop +sound3d +shading +antialias +shadow +dither +switchalt +tshadow +fshadow +lostype +light +rcache +selectable +musicmode +logo +screenchat +gamma +clock +netstats +sing +nometal +noenergy +bigbrother +now +drop +shootall +sharemetal +shareenergy +sharemapping +shareradar +shareall +showranges +setsharemetal +setshareenergy +compression +bps +sfx` |
| CHEATING (2) | `0x00501F48`–`0x00501FC0` | 10 | `+radar +atm +view +los +mapping +doubleshot +halfshot +nowisee +meteor +makeposter` |
| DEBUG (4) | `0x00501FD0`–`0x00502138` | 30 | `+ai +control +kill +iwin +ilose +film +filmspeed +assert +assign +burnall +burnone +debugbreak +dprint +edge +include +mem +memdump +move +printweights +profile +reload +reloadaiprofiles +save +sealevel +search +selboxes +senderror +treedeath +feature +zbuffer` |

**83 commands total**, each with its handler address. Cross-check: TADR independently documents
`0x00501DF4` as the run-level byte of the `+lostype` entry; my walk puts `LOSType` at index 15
of the NORMAL table, i.e. `0x501D38 + 15*12 + 8 = 0x00501DF4`. Exact agreement — the walk is
sound. Note the command *names* live in a separate string pool starting at file `0x100744`
(VA `0x00502144`); that offset is the pool, **not** the table.

`IsCheating` is a `BOOL` at **`0x005091CC`**; `CallInternalCommandHandler(const char*, int)` is
at `0x00417B50`. Registering new commands works in production: TADR hooks `0x004195DD` — the
tail of the engine's own registration function — and calls `InitInternalCommand` with its own
table to add `+autoteam`.

The ~19 dev switches (`-gonzo`, `-memorystatus`, `-performancestatus`, `-debughelper`,
`-dprint*`, `-mem*`, `-fpu*`, `-enableimagehlp*`, `-saveresources`) are string-matched, not
table-driven.

## Why the community uses proxy DLLs instead

Because the sanctioned surface is a dead end and the proxy route is strictly more powerful. The
command table lets you add chat commands, and `DebugHelper.dll` lets you run one function once —
neither gives you code execution *inside* the simulation, structure access, or the ability to
change engine constants. Even to *reach* `InitInternalCommand` you must already be executing
in-process, which means you already needed an injection vector.

The import-rename proxy provides that vector at zero cost: rewrite one same-length string in the
import table (`DDRAW.dll` → `TDRAW.dll` at VA `0x004FF618`) and the loader hands you control
before `main`, with the whole 1.1 MB image mapped at a fixed base and `VirtualProtect` available.
From there TADR runs ~470 annotated addresses and a `static_assert`-checked struct map. The
community abandoned nothing, because there was never anything to abandon.

The modern refinements are worth noting: FunkyFr3sh's **Patch Loader** applies the same byte
patches at runtime so no modified exe need be redistributed; Thaldren's **TA-Hex-Edits** (MIT,
May 2026) instead *expands* the PE to 3 MB with empty space so many more edits fit without
touching DLLs; and Thaldren's **WeaponIDs_UnitTypeIDs** uses a **`dsound.dll`** proxy — a fourth
import slot — to side-load `Patch5.dll`.

## (B) The historical patchers (chronological)

**TA Hook 0.85** — SY_Yeha / clan SY, `welcome.to/yankspankers`, files dated **2000-06-05**.
`hook.dll` exports `_InstallHooks`/`_KillHooks`: a `SetWindowsHookEx` keyboard/mouse automation
tool, explicitly *"not a patch to TA, but rather just a mouseclick simulator"*. F11 fires a chat
macro (`+setshareenergy 1000`, `+setsharemetal 1000`, `+shareall`, `+shootall`, `+noshake`);
Shift+X queues building lines and DT rings. This is the pre-patching era — and the ancestor of
everything after it. `tdraw.txt` still credits it: *"tdraw.dll has a long provenance dating back
to the work of SY_Yeha, who introduced whiteboard markers and the TA Hook and new hotkeys."*

**SY_Yxan's unit-cap patcher** — `patch.bat` dated **1999-08-20**, republished 2010. Not a
fixed-offset patcher: it drives `MALEX.EXE` (a 1997 Borland Swedish search/replace utility) over
two byte patterns, `E8 48 DF 00 00 3D F4 01` → `...3D 88 13` and
`0D E8 1D 51 00 B8 F4 01` → `...B8 88 13`. I located both in stock 3.1c at file `0x90A53`
(VA `0x00491653`) and `0x90A60` (VA `0x00491660`) — which **finally justifies the long-circulated
folklore offsets "0x90A50 and 0x90A60"**. Disassembly shows the routine reads `UnitLimit`, then
`cmp eax,0x1F4 / jle / mov eax,0x1F4` and stores to `TAdynmemStruct+0x37EEC`, with a lower clamp
of `0x14`. So the retail range is **20–500**, and `0x1F4` is a *ceiling*, not the 250 default.
That explains the sibling "500-unit-patch" (652-byte zip) containing **only** a `TOTALA.INI` —
500 needs no exe edit. The "1500-unit-patcher" ships a byte-identical `patch.bat`; both raise the
ceiling to `0x1388` = 5000 and differ only in the shipped ini.

**Xpoy's `ddraw.dll` line & the weapon-ID crack** — TAU's `old-weapon-id-hack/` holds four
Borland-built `ddraw.dll` proxies dated **2002-01-05 … 2002-02-27**, each exporting the full
DirectDraw set. `tdraw.txt` records the lineage: *"Later work by Xpoy extended hotkey support,
added new shortcut commands, uni-code font support, megamap, weapon-id crack (not present in
current release) and porting music patch from GOG version, and later support was provided by
Rime."* These are the direct ancestors of today's `tdraw.dll`. **Xon's DLLs** (2004–2006) are the
`dplayx` side: `Dplayx.dll` (2006-08-19) ships with a 458 KB Delphi **linker map**, strings
`Updated by Xon`, `spank.dll`, `Visit www.clan-sy.com`, `TPluginCommandsU`, and the `.commands`
chat system — i.e. the TA Demo Recorder line, and the origin of the `spank.dll` proxy name.

**bLoBbY (zzymyn) 2004–2006** — four C-source patchers, each with the offsets published in a
sidecar `.txt`. All four **verify byte-exactly against stock 3.1c**:
- `ta_speed_patcher` (**2004-12-13**): `0x901FD` `7E`→`EB` (jle→jmp) and `0x959BE` `73 0E`→`90 90`
  — removes the +10 game-speed cap.
- `+atm patcher` (**2005-01-23**): 4 bytes at `0xFBA7C`, float `-1000.0` → `-10000.0`.
- `+shootall default` (**2005-11-21**): 1 byte at `0xACF6`, `84`→`85` (je→jne).
- `skirmish hack` (**2006-02-25**): four sites replacing the skirmish resource ladder
  (stock: step 500, cap 10000, floor 200) with a doubling ladder. Its `skirmish.txt` prints
  both file offsets and VAs, and its `0x0007A796 → 0047B396` mapping matches the `.text` delta
  `0x400C00` exactly.

**jtaext (click-queue extension)** — J. Rennison, **2006-02-03**. Patch at file `0x19FBB`
(VA `0x0041ABBB`). `jtaext_full.zip` includes `original.bin`, a 256-byte capture of the
pre-patch bytes; it matches stock 3.1c at that offset **byte-for-byte**. `jtaextcfg.exe` writes
a 64-byte binary `jtaext.dat` of eight `{count, delta}` pairs.

**Multicore patch** — `TotalA(Multicore).exe`, **2008-06-18**. Diffed against stock: exactly
**one** code change, `0x1CBA3` (VA `0x0041D7A3`) `32 C0` → `B0 01`, forcing the failure path of
the drive-scan routine at `0x0041D6A0` to return 1 instead of 0 (plus PE header/checksum
churn). Note the community label says "multicore" but the byte is in the CD/drive-detection
routine — I report the byte, not the intent. **[VERIFIED byte / CLAIMED semantics]**

**RomHaxxor & the Pathfinding Fix** — TAU `ta-pathfinding-fix/`, `RomHaxxor.exe` dated
2008-12-13, config dated **2012-03-05**. `pathfinding.cfg` reads
`Pathfind_cycles=0000DED6;0001045A;DWord;Little` — file offset `0xDED6`, new value 66650.
Confirmed: at VA `0x0040EAD6` stock holds `C7 46 48 35 05 00 00` = `mov [esi+0x48],0x535`
(1333) — the `AISearchMapEntries` initialiser. The community patch exe and TA:ESC Gold both
carry the identical `0x1045A` edit.

**Secret Forces** (Shelby123456789, 2017) — **not located**. Absent from TAU's file archive and
mod index; ModDB returns 403 to every client I tried. **[UNVERIFIED]**

**Thaldren's Modular-Patch** (Dec 2025 – 2026, MIT) — a from-scratch `ddraw.dll` (91 KB) that
hooks the DirectDraw COM vtables and blits through the Windows API; fixes DirectX init failure
and non-fullscreen windowing. `DPLAYX` and `WINMM` modules still marked work-in-progress.

**The 4GB / LAA patch** — mechanically, setting `IMAGE_FILE_LARGE_ADDRESS_AWARE` (0x0020) in the
PE `Characteristics` word. In stock 3.1c that field is at **file offset `0xC6`** and reads
`0x010B`. **TA:Escalation Gold ships it as `0x012B`** — the LAA bit set. Every other exe I
examined leaves it clear. **[VERIFIED]**

**TAMusic98 / `win32.dll`** — see below.

## GOG/Steam: the publishers' own hex edit

Verified directly, and pinned. The `WINMM.dll` import-descriptor name string sits at file
**`0xFE442`** (VA `0x004FF642`). Diffing Total Mayhem 1.13's exe (which is built on the digital
re-release base) against stock 3.1c shows a two-byte change at `0xFE445`: `4D 4D` → `33 32`,
turning `WINMM.dll` into `WIN32.dll` — same length, same slot. `win32.dll` intercepts the CD-audio
calls and plays MP3s from `music/`. This is exactly the community's own import-rename technique,
shipped by the commercial re-releases; `winlith/TAMusic98` exists because that DLL is an MSVC
2008 build and will not load on real Windows 98. The community patch does the same thing one
slot over, renaming `WINMM.dll` → `TMUSI.dll` at the identical offset.

## Consolidated offset table

Baseline: stock 3.1c, md5 `f76d367fbbd43008ae04c0f314a61b13`. **`VA − file_offset` is per
section, not constant**: `.text` `+0x400C00`, `.rdata` `+0x401200`, `.data` `+0x401A00`.

| Name | VA | File off | Sec | Controls | Source | Status |
| --- | --- | --- | --- | --- | --- | --- |
| PE `Characteristics` | — | `0xC6` | hdr | LAA bit `0x0020` (4GB patch) | this survey / TA:ESC Gold | VERIFIED |
| `+shootall` default | `0x0040B8F5` | `0xACF5` | .text | `0F 84`→`0F 85` | bLoBbY 2005 | VERIFIED |
| `AISearchMapEntries` init | `0x0040EAD6` | `0xDED6` | .text | `0x535`(1333)→`0x1045A`(66650) | RomHaxxor 2012 | VERIFIED |
| Unit flag branch | `0x00406FB4` | `0x63B4` | .text | `F6 C5`→`EB 40` | TADR exe diff | VERIFIED |
| jtaext hook | `0x0041ABBB` | `0x19FBB` | .text | factory-click handler prologue | jtaext 2006 | VERIFIED |
| `InitInternalCommand` reg. site | `0x004195A7/C9/D3` | `0x189A7/C9/D3` | .text | engine's 3 table registrations | this survey | VERIFIED |
| TADR command-table hook | `0x004195DD` | `0x189DD` | .text | append custom `+commands` | TADR `AutoTeam.cpp` | VERIFIED |
| `CallInternalCommandHandler` | `0x00417B50` | `0x16F50` | .text | `(const char*, int level)` | TADR | CLAIMED |
| CD/data-path check | `0x0041D6B0` | `0x1CAB0` | .text | `75 0E B0 63`→`74 0E B0 2E` | TADR `CrackCdAddr` | VERIFIED |
| Drive-scan fail return | `0x0041D7A3` | `0x1CBA3` | .text | `32 C0`→`B0 01` | Multicore patch 2008 | VERIFIED |
| Forced-success test | `0x004266A5` | `0x25AA5` | .text | `85 C0`→`B0 01` | TADR exe diff | VERIFIED |
| Skirmish +energy ladder | `0x0047B396` | `0x7A796` | .text | step 500 / cap 10000 | bLoBbY 2006 | VERIFIED |
| Skirmish −energy ladder | `0x0047B442` | `0x7A842` | .text | floor 200 | bLoBbY 2006 | VERIFIED |
| Skirmish +metal ladder | `0x0047B52E` | `0x7A92E` | .text | as above | bLoBbY 2006 | VERIFIED |
| Skirmish −metal ladder | `0x0047B5D9` | `0x7A9D9` | .text | as above | bLoBbY 2006 | VERIFIED |
| Speed cap #1 | `0x00490DFD` | `0x901FD` | .text | `7E`→`EB` (jle→jmp) | bLoBbY 2004 | VERIFIED |
| Unit-cap ceiling cmp | `0x00491658` | `0x90A58` | .text | `3D F4 01`(500)→`3D 88 13`(5000) | SY_Yxan | VERIFIED |
| Unit-cap clamp store | `0x00491665` | `0x90A65` | .text | `B8 F4 01`→`B8 88 13` | SY_Yxan | VERIFIED |
| Speed cap #2 | `0x004965BE` | `0x959BE` | .text | `73 0E`→`90 90` | bLoBbY 2004 | VERIFIED |
| Developers-mode gate | `0x00496776` | `0x95B76` | .text | `27 24 25 27`→`24 24 25 0D` (F10) | Patch Loader `patches.ini` | VERIFIED |
| Long-path hook | `0x004CDA44` | `0xCCE44` | .text | `3B C3 0F 85…`→`E9 …` to cave | TADR / TM 1.13 | VERIFIED |
| `.text` slack code cave | `0x004FB92A` | `0xFAD2A` | .text | 21 bytes of appended code | TADR / TM 1.13 | VERIFIED |
| DebugHelper loader | `0x004CBB70` | `0xCAF70` | .text | `LoadLibrary("DebugHelper.dll")` + `DebugFunc1` | this survey | VERIFIED |
| `-debughelper` match | `0x004DA1EE` | `0xD95EE` | .text | switch parse (flag `0x005289CC`) | this survey | VERIFIED |
| `InitInternalCommand` | `0x004B7760` | `0xB6B60` | .text | registers a command table | TADR + `call 0x4b7760` ×3 at reg. site | VERIFIED |
| IAT `GetProcAddress` | `0x004FC094` | `0xFAE94` | .rdata | used by jtaext & DebugHelper | this survey | VERIFIED |
| IAT `FreeLibrary` | `0x004FC0C0` | `0xFAEC0` | .rdata | — | this survey | VERIFIED |
| IAT `LoadLibraryA` | `0x004FC0D4` | `0xFAED4` | .rdata | — | this survey | VERIFIED |
| IAT `ExitProcess` | `0x004FC100` | `0xFAF00` | .rdata | jtaext failure path | this survey | VERIFIED |
| IAT `OutputDebugStringA` | `0x004FC18C` | `0xFAF8C` | .rdata | DebugHelper error path | this survey | VERIFIED |
| IAT `VirtualProtect` | `0x004FC19C` | `0xFAF9C` | .rdata | — | this survey | VERIFIED |
| `+atm` amount (float) | `0x004FCC7C` | `0xFBA7C` | .rdata | `-1000.0` → `-10000.0` | bLoBbY 2005 | VERIFIED |
| Import name — DirectDraw | `0x004FF618` | `0xFE418` | .rdata | `DDRAW.dll`→`TDRAW/TAESC/MDRAW/ZDRAW/spank` | TADR / this survey | VERIFIED |
| Import name — WinMM | `0x004FF642` | `0xFE442` | .rdata | `WINMM.dll`→**`WIN32.dll` (GOG/Steam)** / `TMUSI.dll` | this survey | VERIFIED |
| Import name — DirectPlay | `0x004FF9E4` | `0xFE7E4` | .rdata | `DPLAYX.dll`→`TPLAYX.dll` | this survey | VERIFIED |
| Command table — NORMAL | `0x00501D38` | `0x100338` | .data | 43 entries | this survey | VERIFIED |
| `+lostype` run-level byte | `0x00501DF4` | `0x1003F4` | .data | `1/2/4` | TADR | VERIFIED |
| Command table — CHEATING | `0x00501F48` | `0x100548` | .data | 10 entries | this survey | VERIFIED |
| Command table — DEBUG | `0x00501FD0` | `0x1005D0` | .data | 30 entries | this survey | VERIFIED |
| `+ai` run-level byte | `0x00501FD8` | `0x1005D8` | .data | `4`(DEBUG)→`2`(CHEAT) | TADR exe diff | VERIFIED |
| `+control` run-level byte | `0x00501FE4` | `0x1005E4` | .data | `4`→`2` | TADR exe diff | VERIFIED |
| Command name string pool | `0x00502144` | `0x100744` | .data | ~83 names | this survey | VERIFIED |
| Data path format string | `0x00502916` | `0x100F16` | .data | `"%c:\%s\%s"` → `"%c\\%s\%s"` | TADR / TM 1.13 | VERIFIED |
| Version string | `0x005031B7` | `0x1017B7` | .data | `"v3.1"` → `"v3.9.02"` | TADR exe diff | VERIFIED |
| Ini filename | `0x005098A4` | `0x107EA4` | .data | `"%s\totala.ini"` → `"%s\ta.ini"` | TADR exe diff | VERIFIED |
| `"DebugFunc1"` | `0x0050B524` | `0x109B24` | .data | plugin entry-point name | this survey | VERIFIED |
| `"DebugHelper.dll"` | `0x0050B530` | `0x109B30` | .data | plugin library name | this survey | VERIFIED |
| `IsCheating` (BOOL) | `0x005091CC` | `0x1077CC` | .data | global cheat gate; 5 `.text` refs, first `0x00493E27` | TADR | addr VERIFIED, semantics CLAIMED |
| `TAdynmemStruct**` root | `0x00511DE8` | — | .data | game-state pointer | TADR (seen in SY_Yxan pattern) | VERIFIED |
| `-debughelper` flag byte | `0x005289CC` | — | .data | run-once guard | this survey | VERIFIED |

## Sources

- File Universe (open autoindex): <https://files.tauniverse.com/files/ta/old-stuff-for-v3.1-only/unofficial-patches/>
  — `jtaext.zip`, `jtaext_full.zip`, `atm patcher.zip`, `ta_speed_patcher.zip`,
  `shootall default.zip`, `skirmish hack.zip`, `5kpatch.zip`, `1500patch.zip`,
  `500unitpatch.zip`, `TA_Pathfinding_Fix.zip`, `TAHookv85.zip`, `Xon_dll_files.zip`,
  `ddraw1-2-3.zip`, `ddraw4.zip`, `TotalA(Multicore).zip`
- Cavedog official 3.1c patch (stock baseline exe):
  <https://files.tauniverse.com/files/ta/official-files/official-patches/manual-install/ta1x-31c.zip>
- <https://github.com/tanvanman/TADR> — `src/DDraw/tafunctions.h`, `HardCodeFunctions.cpp`,
  `AutoTeam.cpp`, `TABugFix.cpp`, `tdraw.txt` (provenance/credits)
- <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader> — `res/patches.ini`
- <https://github.com/Thaldren-Updates/Modular-Patch>,
  <https://github.com/Thaldren-Updates/TA-Hex-Edits>,
  <https://github.com/Thaldren-Updates/WeaponIDs_UnitTypeIDs>
- <https://github.com/winlith/TAMusic98> — GOG/Steam `win32.dll` music shim
- GitHub authenticated code search (negative result for `TotalAExt*`)

**Not reachable:** `www.tauniverse.com/forum` (403 — the bLoBbY readmes cite threads 29083,
29596, and posts 524652 / 540318; RomHaxxor cites thread 42529), `moddb.com` (403),
`web.archive.org` (timeout). WebSearch quota was exhausted for this session; Mojeek/Bing/DDG
were captcha-gated, so discovery ran through direct fetch and the GitHub API.
