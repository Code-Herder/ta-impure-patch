# TA Patch Loader, petool & cnc-ddraw (FunkyFr3sh toolchain)

## Summary

`Total-Annihilation-Patch-Loader` is a ~450-line C project building one artefact, `dplayx.dll`, dropped next to a **stock, unmodified TotalA.exe 3.1**. Windows loads it because TA statically imports DirectPlay; its `DllMain` fingerprints the exe, applies byte patches read from an INI embedded as an RCDATA resource, then chain-loads `tdraw.dll` and repoints TA's two `DirectDrawCreate` call sites at it. Every DirectPlay export is a linker forwarder to `tplayx.dll`. Per its README (`tapl/README.md:4`) the point is "to replace the hex edited TotalA.exe in the Total Annihilation community patch" — mods ship an INI, not a modified exe. [VERIFIED]

Companions: `petool` **re-links** a 32-bit PE so you can add genuinely new code (not just overwrite bytes); `cnc-ddraw` is a full DirectDraw reimplementation giving windowed/borderless/upscaled rendering. All three repos cloned at `--depth 50`; every file:line citation below is from those working copies.

## The dplayx proxy mechanism (with code)

TA's import table names `DPLAYX.DLL`, and the Windows loader searches the application directory first (SafeDllSearchMode does *not* protect the app dir), so a `dplayx.dll` in the game folder wins over `C:\Windows\System32\dplayx.dll`. No exe edit, launcher or injector — the stock exe loads your code as a side effect of its own import table. [VERIFIED — standard Windows behaviour; the reliance on it is evident from `exports.def:1` `LIBRARY dplayx.dll` plus a `DllMain`-only design.]

Forwarding is done entirely by the linker, with no thunk code at all (`tapl/exports.def:1-12`):

```
LIBRARY dplayx.dll

EXPORTS
    DirectPlayCreate       = tplayx.DirectPlayCreate       @1
    DirectPlayEnumerateA   = tplayx.DirectPlayEnumerateA   @2
    DirectPlayEnumerateW   = tplayx.DirectPlayEnumerateW   @3
    DirectPlayLobbyCreateA = tplayx.DirectPlayLobbyCreateA @4
    DirectPlayLobbyCreateW = tplayx.DirectPlayLobbyCreateW @5
    gdwDPlaySPRefCount     = tplayx.gdwDPlaySPRefCount     @6
    DirectPlayEnumerate    = tplayx.DirectPlayEnumerate    @9
    DllCanUnloadNow        = tplayx.DllCanUnloadNow        PRIVATE
    DllGetClassObject      = tplayx.DllGetClassObject      PRIVATE
```

These are PE **export forwarders**: the export directory stores the string `"tplayx.DirectPlayCreate"` and the loader resolves it transparently, so real DirectPlay is reached with zero per-call overhead and the correct ordinals (`@1`–`@6`, `@9`) preserved. The same list is repeated as `#pragma comment(linker, "/export:...")` for MSVC builds (`tapl/dllmain.c:100-110`). `tplayx.dll` is the real DirectPlay renamed and shipped in the game folder by the Community Patch installer. [CLAIMED — the rename is not in this repo; only the forwarder target name is verified.] Build is a minimal MinGW cross-compile, `-Wl,--enable-stdcall-fixup -s -shared -static`, i686 (`tapl/Makefile:5-13`).

## Exe fingerprinting & version detection

Two gates, both in `DllMain` (`tapl/dllmain.c:15-41`):

```c
HMODULE game_exe = GetModuleHandleA(NULL);

if (!game_exe || memcmp((char*)game_exe + 0x00010000,
        "\x14\x68\x78\x1B\x50\x00\x8D\x4C\x24\x1B", 10) != 0)
{ /* "Game version not supported. Please install the official 3.1 patch..." */ }
```

So yes: a **10-byte code signature at image base + 0x10000** (= `0x00410000` in `.text`; TotalA.exe has no relocations and always loads at 0x400000). A raw `memcmp` against machine code, not a version string — it pins the *exact* 3.1 build. [VERIFIED]

The second gate catches *older community patches* (`tapl/dllmain.c:7-9, 29`):

```c
#define TA_DDRAW_DLL_STR     ((char*)0x004FF618)
#define TA_MP_VERSION_MAJOR *((BYTE*)0x0049E9C0)
#define TA_MP_VERSION_MINOR *((BYTE*)0x0049E9C9)
...
if (strcmp(TA_DDRAW_DLL_STR, "DDRAW.dll") != 0 &&
    (TA_MP_VERSION_MAJOR != 3 || TA_MP_VERSION_MINOR != 1))
{ /* "Incompatible game files detected. You cannot mix different versions..." */ }
```

`0x004FF618` is the import-table name string for the DirectDraw dependency, which an *old-style hex-edited* Community Patch exe rewrites to point at its own wrapper. The MP version bytes at `0x0049E9C0`/`0x0049E9C9` are the same two the loader itself writes from `MultiplayerVersionMajor`/`Minor`. Together they reject a mixed install. Failures `MessageBoxA` then `exit(1)`. [VERIFIED]

## The named patch corpus (full table: name / address / effect)

INI section names are **raw file offsets** into the original TotalA.exe; `GET_MEM_ADDRESS` (`tapl/patches.h:9-14`) maps them to VAs with per-section deltas — `.text` `0x000400`→`0x00401000` (+0xC00), `.rdata` `0x0FAE00`→`0x004FC000` (+0x1200), `.data` `0x0FF600`→`0x00501000` (+0x1A00); anything ≥ `0x110000` or < `0x400` is rejected. `GET_PHYS_OFFSET` inverts it for error messages. Hence authors keep working straight out of a hex editor. [VERIFIED]

Fourteen named `[Settings]` keys exist — the entire preset vocabulary (`tapl/patches.c:70-312`). Defaults shown are OTA's (`tapl/res/patches.ini`).

| Name | File offset | Memory address | Effect |
|---|---|---|---|
| `RegistryPath` | `0010C3FD`, `001084B8` | `0050DDFD`, `00509EB8` | Replaces `Software\Cavedog Entertainment` tail string; 1–21 chars. Two sites. |
| `ConfigFileName` | `00107EA3` | `005098A3` | Config file name, 1–12 chars (`totala.ini`). |
| `Gp3FileName` | `00100ECC` | `005028CC` | `.GP3` name template, 1–11 chars (`rev%s.GP3`). |
| `DownloadPath` | `00101D30` | `00503730` | Download folder name, 1–11 chars. |
| `GameVersionString` | `001017B4` | `005031B4` | DebugString version text, ≤7 chars. |
| `MultiplayerVersionMajor` | `0009DDC0` | `0049E9C0` | Battleroom protocol major byte, 0–127; all players must match. |
| `MultiplayerVersionMinor` | `0009DDC9` | `0049E9C9` | Battleroom protocol minor byte, 0–127. |
| `CommanderOrdersNotReset` | `000063B4` | `00406FB4` | Writes `EB 40` (jmp short) — AI commander orders no longer reset when attacked. |
| `ChangePathfindingSearch` | `0000DED6` | `0040EAD6` | Writes dword `66650` — raises the initial `+search #` pathfinding limit. Credited to Ti_ and xpoy. |
| `DirectxPopupElimination` | `00025AA5` | `004266A5` | Writes `B0 01` (`mov al,1`) over the DirectX-version test result — kills the startup DirectX warning. |
| `CursorReclaim` | `0003DB86` | `0043E786` | Zeroes a `jz rel32` displacement: reclaim cursor shows over any unit; anonymises Commanders on the minimap. |
| `DisableKeyLastCommandRepeat` | `00095AE7` | `004966E7` | Byte `0x27` in the keyboard action table — frees `\` as the demo-recorder whiteboard key. |
| `EnableF10Debug` | `00095B76`, `00095B79` | `00496776`, `00496779` | Bytes `0x24`, `0x0D` in the same table — F10 debug (needs dev mode); `Insert` repeats last command. |
| `IncreaseAtmToFillResources` | `000FBA7F` | `004FCC7F` | Float constant high byte `C4`→`D4`: `-1000.0` → `-1000·2^32` (≈ `-4.295e12`), so `+atm` fills metal and energy regardless of storage. Credited to N72. |

Anything else is a `[<hexfileoffset>]` / `patch=<hex bytes>` section handled by `patches_apply_customs` (`tapl/patches.c:314-390`): whitespace-tolerant hex, single digits rejected, 512-byte cap. OTA's `patches.ini` carries exactly two such raw patches, both labelled `; UNKNOWN`: `[001005D8] patch=02` and `[001005E4] patch=02` (VAs `0x00501FD8`, `0x00501FE4`).

Two source-comment discrepancies: `tapl/patches.c:211` says `(0040EAC9)` where the computed address is `0x0040EAD6` (the INI is right); `tapl/patches.c:294` says `(004FCC7C…)`, the start of the float whose last byte at `...7F` actually changes. [VERIFIED]

## Per-mod patch sets

`res/prota.ini` (ProTA 4.5, 70 sections) and `res/mayhem.ini` (Total Mayhem 10.9.2, 158 sections) are drop-in replacements for `res/patches.ini`; `res/res.rc:1` embeds exactly one as resource `1000`, so a per-mod `dplayx.dll` is a rebuild, not a runtime choice. Both remap game identity — registry path (`ProTA` / `TotalM`), config, GP3, download folder, MP version (4.5 / 10.9) — so a mod install cannot join a vanilla lobby.

ProTA's byte patches (each documented with full before/after hex rows) cover: `init_cloaked` units not cloaking until built; weapons acquiring targets while attacking, DGUNning ground, and via resurrection units; reclaim-sound fixes for ground and VTOL constructors; the "Ressurection failed" spelling fix; AI unit-count threshold `05`→`0A`; AI stockpile purchase fix; a large *eliminate targetlocking* rewrite (`00007E20`–`00007FA2`); full scoreboard for departed players; Allied Victory on by default; the OTA set's DirectX-popup, cursor-reclaim, `\`-key, F10 and `+atm` patches; and AI group 5 redirected to group 1 (`[000FB780] patch=D0 86`).

Total Mayhem is a superset in spirit: difficulty-scaled AI income and reclamation multipliers (hard 1.5×, medium 1.0×, easy 0.5×), a long family of "acquire targets while X" patches spanning build/assist/reclaim/repair for ground and VTOL, hold-position guard behaviour, submerged-target and `setSFXoccupy` fixes, a Rahsennor-authored **teleport button**, `healtime` and CTRL-Z fixes, plus folder redirections (`guiM`, `unitsM`, `weaponM`, `unitpicM`) and three `; STILL UNKNOWN` bytes. Detail belongs to the sibling ProTA / Total Mayhem notes.

## Conflict detection

`patches_setbytes` (`tapl/patches.c:12-58`) keeps a static `PATCH_OFFSET g_patches_offsets[4096]` of `{start, end}` VA ranges. Before writing it scans every occupied entry for overlap with `[offset, offset+size)` — three interval tests covering start-inside, end-inside and straddling — and on a hit logs `"Patch '%08X (%08X)' is conflicting with:\nPatch '%08X (%08X)'"` in both file-offset and VA form, returns 0, and aborts. Returning 0 from the inih callback makes `ini_parse_string` return the offending **line number**, surfaced as `"Failed to apply game patches. Error on line %d."` (`tapl/dllmain.c:43-58`). Empty patches and offset 0 are rejected too. Commits `193d9c6 detect conflicting patches` and `acd5fea`/`3b05f20 remove conflicting patches` show it immediately found real overlaps in the mod INIs.

Caveat: only patches routed through `patches_setbytes` are tracked. The `DllMain` writes — `patch_setbyte((void*)0x00401064, 1)` (a code-cave byte telling `tdraw.dll` the proxy is live) and the two `patch_call` sites — bypass the table, so a mod INI touching those would silently collide. [VERIFIED] `petool patch` has the same table (`petool/src/patch.c:15-18, 55-70`) but only *warns*.

## petool: adding new code to a 32-bit PE

petool does **not** append a section to an existing exe — it takes the problem the other way round. `genprj` (`petool/src/genprj.c`) copies `TotalA.exe` → `TotalA.dat`, then:

1. `pe2obj` strips the DOS stub and PE signature (`PointerToRawData -= e_lfanew + 4`) so the remaining bytes *are* a valid i386 COFF object (`petool/src/pe2obj.c:31-58`);
2. `genlds` emits a GNU ld script pinning every original section back at `ImageBase + VirtualAddress`, discarding `.rsrc`/`.reloc` (replaced by `FILLn` gaps so layout holds), rebuilding `.idata` from the original import descriptors as raw `LONG`s, then adding **new** sections `.p_text`, `.p_data`, `.p_rdata`, `.p_bss`, `.p_tls` after them (`petool/src/genlds.c:100-290`);
3. `gensym` writes `sym.cpp` full of `SETCGLOB(0x…, name)` so your C can call original functions and imports by name (`petool/src/gensym.c:52-106`);
4. `genmak` writes a Makefile that links with `-Wl,--disable-reloc-section --enable-stdcall-fixup -static`, restores every DataDirectory ld got wrong via `setdd`, forces `.p_text` to `0x60000020` (CODE|EXECUTE|READ) via `setsc`, runs `petool patch`, then `strip -R .patch` (`petool/src/genmak.c:349-357`).

New code lands in a real section with real symbols and unlimited space; the *hooks* are declarative macros assembling into a `.patch` section (`.section .patch,"d0"` + `.long addr` + `.long len` + bytes — `petool/res/inc/macros/patch.h:3-11`), which `petool patch` applies by RVA→file-offset lookup and `strip` then removes.

Concretely, for a feature needing more space than the bytes it replaces: drop `petool.exe` on `TotalA.exe`, `make` once to prove a clean relink, then write `EXTERN_C void myFeature(...)` and hook it with `CALL(0x004xxxxx, _myFeature)` (one 5-byte call; `CALL_NOP` for 6), `HOOK(addr, end)` for a naked jmp with `pushad`/`popad` and a `jmp back`, or `DETOUR(start, end, _myFn)` to replace a whole function (INT3-fills the original). Restore clobbered instructions with `TRAMPOLINE(addr, original, "…")` in `sym.cpp`. `genpatch` converts a pair of (original, hex-edited) exes into `SETBYTES(0x…, "\x..")` macros, so a legacy byte-patch corpus ports mechanically (`petool/src/genpatch.c:165-177`).

## cnc-ddraw and the single-ddraw-slot problem

cnc-ddraw is a from-scratch DirectDraw implementation exporting the real ordinals (`cncddraw/exports.def`: `DirectDrawCreate @8`, `DirectDrawCreateEx @10`, `AcquireDDThreadLock @1` …) plus extras `DDIsWindowed`, `DDEnableZoom`, `DDGetProcAddress`, `GameHandlesClose`. TA is explicitly listed as supported (`cncddraw/README.md:445-447`, including Unofficial Beta Patch v3.9.02); it gains GDI/OpenGL/D3D9 renderers with auto-selection, windowed / borderless / exclusive fullscreen with Alt+Enter, GLSL and libretro shader upscaling, FPS limiter, VSync, saved window geometry, mouse-sensitivity scaling. Presenting a normal window also makes it trivially OBS-capturable. [VERIFIED for features/TA support; OBS is an inference, not a source claim.]

There is only one `ddraw.dll` slot, and cnc-ddraw refuses to share it: `util_caller_is_ddraw_wrapper` (`cncddraw/src/utils.c:268-385`) resolves the exe's IAT entries for `DirectDrawCreate`/`DirectDrawCreateEx`, asks `GetModuleHandleExA` which module owns them, and if the answer is `D3dHook.dll`, `wndmode.dll`, `windmode.dll`, `dxwnd.dll` or Voobly's `age.dll`, pops a "You cannot combine cnc-ddraw with other DirectDraw wrappers" box and returns `DDERR_GENERIC` (`cncddraw/src/dllmain.c:219-239`).

The Community Patch sidesteps the collision by **not** using the ddraw slot for its own wrapper. `tdraw.dll` is loaded by name and its `DirectDrawCreate` is patched directly into TA's two call sites (`tapl/dllmain.c:65-94`):

```c
HMODULE tdraw_dll = LoadLibraryA("tdraw.dll");
FARPROC dd_create = GetProcAddress(tdraw_dll, "DirectDrawCreate");
patch_call((void*)0x0047BFA2, (void*)dd_create);
patch_call((void*)0x004B55FB, (void*)dd_create);
```

`patch_call` (`tapl/patch.h:6-15`) overwrites a 5-byte `E8 rel32` and returns the *original* target, redirecting the site without touching the import table — which is why the fingerprint check still expects `"DDRAW.dll"` intact at `0x004FF618`. `tdraw.dll` then does the DirectDraw work and forwards to whatever occupies `ddraw.dll`, so cnc-ddraw can sit underneath in that slot and both coexist. `patch_setbyte((void*)0x00401064, 1)` (`tapl/dllmain.c:62`) is the handshake byte tdraw reads to know the proxy is active. `edraw.dll` appears nowhere in these three repos. [VERIFIED: tdraw chain-load and call-site patching. CLAIMED: that tdraw forwards to `ddraw.dll` — tdraw's source is not in these repos.]

## Reusability for a new project

All three are permissive: Patch Loader **MIT** (`tapl/LICENSE`, © 2023 FunkyFr3sh), cnc-ddraw **MIT** (© 2025), petool **MIT** top-level (© 2024) with the whole `res/` template tree — the macro headers, `patch.h`, `app.h`, the proxy scaffolding you would actually paste into your project — released as **BSD0** (`petool/res/LICENSE`): no attribution required. Bundled inih carries its own BSD licence; older petool files bear Toni Spets' ISC-style notice.

Practical starting posture for a new TA patch project:

- **Do not fork the exe.** Take the Patch Loader wholesale, swap `res/patches.ini`, rebuild — ~450 lines of readable C plus inih.
- **Keep INI offsets as file offsets** and let `GET_MEM_ADDRESS` map them; that preserves hex-editor workflow and compatibility with the community's archaeology.
- **Extend the conflict table** to cover your `DllMain`-time writes too, and consider a layered INI so a mod composes onto vanilla rather than replacing it.
- **Reach for petool the moment a patch needs more bytes than it replaces**; `genpatch` imports a legacy hex-edit corpus mechanically.
- **Leave `ddraw.dll` free** for cnc-ddraw; use the `tdraw.dll`-style named-DLL + call-site-patch route for your own rendering hooks.
- Hard constraint: this targets *exactly* OTA 3.1, no relocations, base `0x00400000`. Any other build fails the `memcmp` at `+0x10000` and exits.

## Sources

- `github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader` — `dllmain.c`, `patches.{c,h}`, `patch.h`, `exports.def`, `Makefile`, `res/*`, `LICENSE`, git log.
- `github.com/FunkyFr3sh/petool` — `README.md`, `src/{genprj,genlds,genmak,gensym,pe2obj,patch,setdd,setsc,genpatch}.c`, `res/inc/macros/patch.h`, `res/src/winmain.cpp`, `res/proxy/readme.txt`, `LICENSE`, `res/LICENSE`.
- `github.com/FunkyFr3sh/cnc-ddraw` — `README.md`, `exports.def`, `src/{dllmain,utils}.c`, `Makefile`, `LICENSE`.
- INI attributions cite tauniverse.com thread `t=41608` (pp. 4, 6); authors Ti_, xpoy, N72, Rahsennor.
