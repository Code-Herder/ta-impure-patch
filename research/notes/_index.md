## The short answer

Total Annihilation's source was never released. Every "engine feature" the community has added
since 1998 is therefore a modification of the shipped 1997 binary — and the scene converged, twice
and independently, on **one technique**:

1. **A same-length hex edit of one string in `TotalA.exe`'s import table.** The import-descriptor
   name `DDRAW.dll` at VA `0x004FF618` (file offset `0x000FE418`) is overwritten with a
   mod-specific five-letter stem — `TDRAW.dll`, `TAESC.dll`, `MDRAW.dll`, `ZDRAW.dll`. The
   five-letter convention is not style; it is what "same length as `ddraw`" means.
2. **A proxy DLL of that name** re-exports the real DirectDraw API, and then in `DllMain` installs
   inline hooks and constant patches into the running game with `VirtualProtect`, before
   chain-loading a genuine DirectDraw wrapper (cnc-ddraw).
3. **Two sibling proxies** do the same for the other Cavedog-era imports: `DPLAYX.dll` →
   `TPLAYX.dll` (networking and the replay recorder) and `WINMM.dll` → `TMUSI.dll` (CD audio → MP3).

Since 2023 there is a **hex-edit-free** variant: FunkyFr3sh's Patch Loader ships *as* `dplayx.dll`,
is loaded by the stock exe's own import table, and applies the identical byte patches at runtime —
so no modified executable has to be redistributed. The scene is actively migrating to it; Total
Mayhem 11.3.0 already ships no patched exe at all.

## Why the binary was so tractable

`TotalA.exe` is a plain, unpacked MSVC 5.0 build with no ASLR and a fixed image base of
`0x400000`, which is why absolute addresses have stayed valid for twenty-five years. It still
carries its PDB path (`C:\cavedog\wargame\Release\TotalA.pdb`), several original source filenames,
around nineteen debug command-line switches, and an **83-entry developer command table** in three
NULL-terminated blocks — 43 NORMAL at `0x00501D38`, 10 CHEAT at `0x00501F48`, 30 DEBUG at
`0x00501FD0` — registered through `InitInternalCommand` at `0x4B7760`. That registration function
is the closest thing to a sanctioned extension point, and modern patches do use it to add new
commands.

## What is genuinely added, and where it lives

The single most reusable technique in the whole corpus is how new unit properties are supported
**without writing a parser**: the patch hooks the definition loader mid-parse and calls TA's *own*
`TdfFile::GetInt` / `GetFloat` / `GetString` (`0x4C46C0` / `0x4C4760` / `0x4C48C0`) with keys the
1997 code never thinks to ask for. The engine's parser is repurposed to read a vocabulary it was
never taught — which is how `nottoair`, `surfacefire` and the veterancy FBI keys exist at all.

The second lesson is that a surprising amount was never missing. Cavedog left the **entire teleport
feature** finished and disconnected inside the retail exe: the `teleporter` FBI tag is parsed into
a unit-def bitfield, the order-descriptor table at `0x004FCEE8` carries a populated entry 12, and
the handler at `0x00406AA0` is complete working code. Reconnecting it took roughly **40 bytes**.

## Myths this research overturned

| Common claim | What the evidence shows |
|---|---|
| Mods add **shield projectors** as an engine feature | No shipped mod does. TA:ESC and TA Zero both implement shields in stock 1997 COB script — measuring damage by differencing `get HEALTH`, then toggling `set ARMORED`. A real engine shield (`ShieldRange`, network-synced) exists in TADR's source and is used by nobody. |
| Cavedog shipped a **`TotalAExt` plugin ABI** | It does not exist — zero hits across seven exe builds, every community DLL, and GitHub code search. The one real optional-DLL hook is `DebugHelper.dll`, gated behind `-debughelper`. |
| TA:ESC is an **engine-patching project** | Its `TAESC.dll` is byte-identical to upstream TADR's `tdraw-escalation` build. Wotan specifies features; the TADR maintainers implement them. |
| The **6-button build menu** was widened by patching | The build menu was always data-driven `.GUI` gadget lists. No patch touches a grid. |
| Teams independently reverse-engineered their patches | 67 of ProTA's 69 addresses also appear in Mayhem's set, 64 byte-identical. It is one shared community corpus. |
| **Zwzsg** was a prominent exe hacker | Every primary source credits him for COB *scripting*. No evidence of binary work. |

## The load-bearing risk

Every project here pins hard-coded absolute addresses — TADR's memory map alone carries roughly 470
of them. That is precisely the failure mode that froze StarCraft's BWAPI on version 1.16.1 for nine
years. The known counter-pattern is to *derive* addresses by static analysis and expose symbolic
identifiers to plugins, as the `scarf`/`samase` stack does for Brood War. No TA project does this
yet.

## Where to start

If you want the mechanism, read **The core mechanism** then **TADR / tdraw.dll**. If you want to
build something, read **TA Patch Loader** (the hex-edit-free foothold and `petool`) and
**Playbooks from other games** (how other communities kept such projects alive). Every claim in
this wiki is tagged <span class="pill pill-ok">VERIFIED</span> where a primary artifact was read
directly, or <span class="pill pill-warn">CLAIMED</span> where it rests on community assertion.
