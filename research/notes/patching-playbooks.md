# Playbooks: How Other Communities Added Engine Features to Closed-Source RTS Binaries

*Scope: adding real engine features to a shipped, closed-source 1990s/2000s RTS `.exe`. Engine
re-implementations (Spring/Recoil, OpenRA, TA3D) are deliberately out of scope. Every substantive
claim is marked **[VERIFIED]** (read directly in source code or primary documentation) or
**[CLAIMED]** (asserted on a forum/wiki/secondary page and not independently confirmed).*

## Summary

Three mature mechanisms exist for adding engine features to a closed-source RTS binary, in
ascending order of capability and of distribution difficulty:

1. **Static byte patch on the user's own file** — AoE2 UserPatch. An installer rewrites bytes in
   `age2_x1.exe` in place. Zero runtime dependency, perfect launcher/lobby compatibility; but
   unmaintainable by anyone but the author (UserPatch's source was never released) and it produces
   a modified binary you must not redistribute.
2. **Runtime injection into an untouched exe** — RA2's Syringe + Ares + Phobos, Tiberian Sun's
   Vinifera, and TA's own `Total-Annihilation-Patch-Loader`. A loader or proxy DLL rewrites call
   sites *in memory* at load time and jumps into C++ functions exported from a DLL. The file on
   disk is never modified, so there is nothing copyrighted to redistribute. **This is the model to
   copy.**
3. **Decompile-to-object and re-link** — FAF's `FA-Binary-Patches` and the general-purpose
   `FunkyFr3sh/petool`. Convert the PE back to a COFF object, add new code as new PE sections,
   re-link with GNU ld. Most capable; forces you to solve distribution (FAF answered first with
   bsdiff against the user's own copy, now with Steam/GOG ownership verification).

The decisive lessons are not the injection trick — that part is a weekend of work:

- **The surviving projects turned their engine hooks into a data-driven API** (new INI tags, new AI
  script commands) so modders could consume features without touching assembly. Projects that only
  shipped hardcoded features died with their author.
- **Every one of them first built a header library** binding C++ names to the game's structs and
  functions at fixed addresses (YRpp for RA2, `Offsets.h` for Brood War). Everything downstream —
  contributors, features, tooling — depends on that artifact existing.
- **For a lockstep RTS, inject orders, not state.** BWAPI drives an entire AI-competition ecosystem
  through StarCraft's own per-turn command queue and never desyncs.
- **Hardcoded addresses are what kills these projects.** BWAPI is frozen on Brood War 1.16.1 nine
  years after 1.18 shipped because its offsets are literals; the Remastered-era answer *derives*
  addresses by static analysis and hands plugins symbolic ids instead.

---

## AoE2 UserPatch

**What it is.** A "feature and bug-fix update for Age of Empires II: The Conquerors Expansion, with
a primary focus on correcting several long existing issues with the game, the AI system,
compatibility, and related elements" **[VERIFIED]**
(<https://userpatch.aiscripters.net/>). Current release is v1.5 RC build 6268, dated 2019-02-28,
with v1.2 (2013), v1.3 (2013) and v1.4 (2015) before it **[VERIFIED]** (same page) — roughly a
decade of active development on a 1999 binary.

**Feature class delivered.** Population caps to 250 or 1000; all video resolutions including
widescreen; 60fps single-player rendering (vanilla is 20fps); live spectating of single- and
multiplayer games; a "Team Random" civ option; a new Relics victory condition; multiple building
queue; **over 50 new AI scripting commands**; weather effects; animated water **[VERIFIED]**
(<https://userpatch.aiscripters.net/>). This is genuine engine work, not asset swapping.

**Mechanism: static byte patch applied client-side.** The installer `SetupAoC.exe` runs inside the
user's game folder and either **updates `age2_x1.exe` in place** or **creates a separate
`age2_x1.5.exe`**; it also drops `wndmode.dll` (windowed mode) and `miniupnpc.dll` (UPnP) next to
the exe, and requires a pre-existing valid Conquerors v1.0c install (no no-CD) **[VERIFIED]**
(<https://userpatch.aiscripters.net/guide.html>). So: static patch for engine changes, ordinary
DLLs for self-contained services.

Two pieces of direct evidence for the "byte patch" characterisation **[VERIFIED]**: the installer
takes per-feature flags — `SetupAoC.exe -i:FLAGS`, one 0/1/2 digit per feature (widescreen command
bar, window mode, UPnP, extended pop caps, multi-queue, "disable multiplayer anticheat", …), so one
distributable produces many binary configurations
(<https://github.com/SiegeEngineers/userpatch-notes/blob/master/notes/installation.md>); and a
quoted **scripter64** post tells mod developers wanting to undo a fix to "change `0x00127125` (file
space) to `8B0DA0127900`" — a raw file offset and replacement bytes
(<https://github.com/SiegeEngineers/userpatch-notes/blob/master/notes/modding.md>, quoting
<https://forums.aiscripters.com/viewtopic.php?f=3&t=2318&p=61658#p61658>).

**Source availability.** No public source repository exists. Community assertions are that UserPatch
was written entirely in hand-authored x86 assembly by essentially one person (handle
*scripter64*, also published as *xOmicron*) **[CLAIMED]** — this is repeated across AoEZone and AoKH
threads but I could not fetch a primary quote (aoezone.net returns HTTP 403 to automated fetches),
so treat "pure assembly" and "solo" as unconfirmed. What *is* verifiable is that the community
had to **reverse-engineer UserPatch itself** afterwards: `SiegeEngineers/userpatch-notes` exists
precisely to document "undocumented or underdocumented UserPatch things" across Installation,
Modding, Multiplayer, Random Map, Recorded Game and Spectator areas **[VERIFIED]**
(<https://github.com/SiegeEngineers/userpatch-notes>). That is the cost of a closed-source patch.

**A concrete example of how it added features without breaking the wire format.** UserPatch shrank
the `m_sGameFilename` field of the DirectPlay lobby `AGEPresetData` struct from 260 bytes (AoC
1.0c) to 240 bytes and reused the 20 freed bytes for four whitelisted spectator IPs plus a reserved
flags dword (used for Hidden Civs) **[VERIFIED]**
(<https://github.com/SiegeEngineers/userpatch-notes/blob/master/notes/multiplayer.md>). This is the
canonical old-engine trick: **find slack inside existing fixed-size structures rather than growing
them**, so length-sensitive code and lobby plumbing keep working.

**Multiplayer version matching.** UserPatch's installer marks features with a `(sync)` tag, and the
documentation states: *"The (sync) mark indicates that a feature will affect multiplayer game setup
or synchronization. Everyone in a game must have these types of features installed, or not
installed, or you will experience errors."* **[VERIFIED]**
(<https://userpatch.aiscripters.net/guide.html>). Note what this is: the patch itself does **not**
enforce anything. It classifies features into sync-affecting and cosmetic, and pushes enforcement
outward to the player or to the lobby service. In practice enforcement came from **Voobly**, the
third-party matchmaking service, which distributes UserPatch as a "game mod" with its own patch and
anti-cheat layer **[CLAIMED]** (<https://www.voobly.com/gamemods/mod/51/UserPatch>). Separating
"cosmetic, safe to differ" from "sync-critical, must match" is the transferable idea.

**Distribution & legality.** The installer is shipped alone and applied to the user's own legally
obtained v1.0c install; no Microsoft/Ensemble binary is redistributed **[VERIFIED]**
(<https://userpatch.aiscripters.net/guide.html>).

**Making it data-driven — the part that mattered.** UserPatch's most durable contribution is the
extended scripting surface: searchable AI-command, Strategic-Number and Random-Map-command
references, all consumable from plain text script files **[VERIFIED]**
(<https://userpatch.aiscripters.net/reference.html>, <https://airef.github.io/>). Because "50+ new
AI commands" is a *text API*, a generation of AI scripts and total conversions were built by people
who never opened a disassembler.

**Modern successor mechanism worth stealing.** `SiegeEngineers/aoc-mmmod`, "a mini DLL mod loader
for Age of Empires 2: The Conquerors", is a **proxy DLL impersonating `language_x1_p1.dll`**: the
loader is placed at `…\Data\language_x1_p1.dll`, the real language file is renamed
`language_x1_m.dll`, and the loader initialises, loads mod DLLs, then forwards to the real library.
Mod DLLs export a four-function lifecycle — `mmm_load()`, `mmm_before_setup()` (where hooks are
installed), `mmm_after_setup()`, `mmm_unload()` — and it requires UserPatch 1.5 **[VERIFIED]**
(<https://github.com/SiegeEngineers/aoc-mmmod>). `withmorten/dsound`, "a dll loader for age2_x1",
plays the same trick against `dsound.dll` **[VERIFIED]** (<https://github.com/withmorten/dsound>).
The **proxy-DLL pattern is the lowest-effort code-injection route for any 90s Win32 game**: the game
already loads something you can stand in front of.

---

## RA2: Syringe + Ares + Phobos

This is the strongest model for TA and the mechanism is fully open-source, so it can be described
exactly rather than guessed at.

### Syringe: the injector

`Syringe.exe` "allows the injection of code from a DLL into a process it started" **[VERIFIED]**
(<https://github.com/Ares-Developers/Syringe>). Usage is
`Syringe.exe "gamemd.exe" [args for gamemd.exe]` **[VERIFIED]**
(<https://phobos.readthedocs.io/en/latest/>). The exe on disk is never modified — the Ares docs
state Syringe "injects DLL code into a running executable without modifying the executable file
itself" **[VERIFIED]** (<https://ares-developers.github.io/Ares-docs/index.html>).

**The mechanism, read from source** (`SyringeDebugger.cpp` / `.h`,
<https://github.com/Ares-Developers/Syringe>) **[VERIFIED]**:

1. **Parse the target PE before launching.** `RetrieveInfo()` opens the exe, records its
   `TimeDateStamp`, computes its file size and a **CRC32 of the whole file**, resolves the entry
   point from `AddressOfEntryPoint + ImageBase`, and walks the import table to find the addresses of
   `KERNEL32.DLL!LoadLibraryA` and `KERNEL32.DLL!GetProcAddress` — it will reuse the game's own
   imports to load hook DLLs.
2. **Discover hook DLLs.** `FindDLLs()` scans `*.dll` in the game directory. For each, it looks for
   a PE section named **`.syhks00`**; if present it parses an array of 16-byte `hookdecl`
   records `{ unsigned hookAddr; unsigned hookSize; const char* hookName; }`. If there is no such
   section it falls back to a plain-text sidecar file named `<dll>.inj`.
3. **Verify the DLL is meant for this exe.** Either the DLL exports
   `SyringeHandshake(SyringeHandshakeInfo*)` — in which case Syringe passes it `num_hooks`, a
   checksum over all hook addresses/sizes, and the exe's `exeFilesize`, `exeTimestamp` and `exeCRC`,
   and the DLL returns `S_OK` to accept — or the DLL carries a **`.syexe00`** section of
   `hostdecl { unsigned hostChecksum; const char* hostName; }` records and Syringe checks the host
   name matches the exe filename (`CanHostDLL`).
4. **Run under a debugger, break at the entry point.** The process is started debugged with an
   `INT3` (`0xCC`) at the entry point.
5. **Build a trampoline per hook address.** At the entry-point breakpoint, for every hooked address
   Syringe allocates memory in the target and writes this exact stub, once per hook registered at
   that address:

   ```
   60 9C                       PUSHAD, PUSHFD
   68 <hookAddr>               PUSH HookAddress
   54                          PUSH ESP
   E8 <rel32 to hook proc>     CALL ProcAddress
   83 C4 08                    ADD ESP, 8
   A3 <&ReturnEIP>             MOV ds:ReturnEIP, EAX
   9D 61                       POPFD, POPAD
   83 3D <&ReturnEIP> 00       CMP ds:ReturnEIP, 0
   74 06                       JZ .proceed
   FF 25 <&ReturnEIP>          JMP ds:ReturnEIP
   ```

   then it appends the **original overridden bytes** (`hookSize` of them, read out of the live
   process) and an `E9 rel32` **jump back to `hookAddr + 5`**. Finally it patches the original site
   with `E9 rel32` to the stub, NOP-padding out to `hookSize`.
6. **Detach.** Once hooks are placed the debugger detaches and the game runs at full speed
   **[VERIFIED]** (<https://github.com/Ares-Developers/Syringe>).

The contract for a hook function follows directly from that stub and is spelled out in
`YRpp/Syringe.h` **[VERIFIED]**
(<https://github.com/Ares-Developers/YRpp/blob/master/Syringe.h>):

- Signature: `extern "C" __declspec(dllexport) DWORD __cdecl Name(REGISTERS* R)` — the
  `EXPORT_FUNC(name)` macro.
- `REGISTERS` is the `PUSHAD`/`PUSHFD` frame reinterpreted as a struct, giving read/write access to
  `EAX`–`EDI`, `ESP`, `EBP`, `EFLAGS`, the 16-bit and 8-bit sub-registers, plus helpers
  `Stack<T>(offset)`, `Base<T>(offset)`, `lea_Stack<T>(offset)`.
- **Return value is the mechanism for returning control**: return `0` to fall through (the stub
  replays the original bytes and jumps back to `hookAddr+5`); return any **absolute address** and
  the stub `JMP`s there. This is how a hook skips, redirects, or replaces vanilla code.
- Declaration and function opening are fused into one macro:
  `DEFINE_HOOK(address, funcname, size)`, plus `DEFINE_HOOK_AGAIN` for registering the same function
  at a second address. `declhook` places the `hookdecl` into `.syhks00` via
  `__declspec(allocate(".syhks00"))`; `declhost(exename, checksum)` places a `hostdecl` into
  `.syexe00`.

A real hook from Phobos, showing all of it at once **[VERIFIED]**
(<https://github.com/Phobos-developers/Phobos/blob/develop/src/Ext/Anim/Hooks.cpp>):

```cpp
DEFINE_HOOK(0x423B95, AnimClass_AI_Early, 0x8)
{
    GET(AnimClass* const, pThis, ESI);
    ...
    return 0x423BC8;          // redirect execution
}

DEFINE_PATCH(0x424538, 0x8B, 0x8E, 0xCC, 0x00, 0x00, 0x00);  // neuter Ares' hook here
DEFINE_HOOK(0x42453E, AnimClass_AI_Damage, 0x6) { ... }
```

Note `DEFINE_PATCH` — a raw byte overwrite — sitting alongside `DEFINE_HOOK`, and the comment
"*Nuke Ares' animation damage hook at 0x424538*": two independent extension DLLs coexisting in one
process and having to negotiate over the same addresses.

### Ares and Phobos: the feature layers

**Ares** is "the new tool to extend the capabilities of Yuri's Revenge and to fix bugs in the game
engine", conceived in 2007, released publicly in 2010 **[VERIFIED]**
(<https://ares-developers.github.io/Ares-docs/index.html>). It targets `gamemd.exe` from Yuri's
Revenge **1.001**, supporting the patched CD version, The First Decade and The Ultimate Collection
**[VERIFIED]** (same). Feature categories: bug fixes, restored Tiberian Sun logic, new/enhanced
in-game logic, UI features, building enhancements, weapon/warhead/projectile options, Tiberium
mechanics, superweapon customisation **[VERIFIED]** (same). The docs are explicitly aimed at "mod
authors wishing to make use of the new functionality that Ares offers", primarily through **INI
configuration tags** **[VERIFIED]** (same). Repo: <https://github.com/Ares-Developers/Ares>,
created 2011-10-17, ~167 stars, 7 GitHub contributors, last push 2025-08 **[VERIFIED]** (GitHub API).

**Phobos** is "a community engine extension project providing a set of new features and fixes for
Yuri's Revenge", built on modified YRpp and **SyringeEx** (an extended, open-source fork of Syringe)
**[VERIFIED]** (<https://phobos.readthedocs.io/en/latest/>, <https://github.com/Phobos-developers/SyringeEx>).
It is **independent of Ares and does not require it**, but is designed not to conflict and Ares is
"highly recommended" for the full feature set **[VERIFIED]** (same). Repo:
<https://github.com/Phobos-developers/Phobos>, created 2020-08-16, ~447 stars, **42 GitHub
contributors**, still being pushed to in August 2026 **[VERIFIED]** (GitHub API). Phobos is the
proof that this architecture *scales past its founders* — six times Ares' contributor count, on a
2001 binary, 25 years later.

**Hook-conflict management is a first-class engineering problem here.** Phobos ships an internal
tooling skill, `.agents/skills/check-hooks`, whose job is to validate every new
`DEFINE_HOOK`/`DEFINE_HOOK_AGAIN` against a reference list of *Ares'* hooks
(`ares_3.0p1_hooks.cpp`, and a `HookAnalysis.txt` report from "SyringeIH"). It checks for hooks with
`size < 5` (too small for the `E9 rel32` patch — requires verifying the trailing bytes are NOPs),
address conflicts with existing hooks, instruction-boundary misalignment, wrong register-access
macros, and relative instructions inside the overridden range **[VERIFIED]**
(<https://github.com/Phobos-developers/Phobos/blob/develop/.agents/skills/check-hooks/SKILL.md>).
**Anyone starting a TA equivalent should plan for this on day one: a machine-readable registry of
every hooked address and its overridden byte count.**

### Multiplayer and the network layer

`CnCNet/yrpp-spawner` is "CnCNet DLL for Command and Conquer: Yuri's Revenge using Syringe"
**[VERIFIED]** (<https://github.com/CnCNet/yrpp-spawner>). It lets the CnCNet client launch the game
straight into a battle, skipping menus, configured through a `spawn.ini` file; it is built with the
same YRpp framework as Ares/Phobos and is loaded as a Syringe-compatible DLL (SyringeEx recommended,
original Syringe supported). It is a rewrite of CnCNet's earlier YR patches. Its feature list
includes **event verification checks, desync fixes, multiplayer save/load and autosave**
**[VERIFIED]** (same). Licensed GPL-3.0, binaries via GitHub releases, and it states plainly that it
is "an unofficial open-source community collaboration project" with no EA affiliation **[VERIFIED]**
(same). The transferable point: **the matchmaking/netcode layer is just another hook DLL**, written
by a different team, on the same injector.

**Version-matching posture.** Ares refuses to inject into an executable it does not recognise:
"Ares is not intended to be used in conjunction with any third party patch that modifies the
executable of the game", because Ares injects "at very specific points, and modifications to the
executable change the locations of these points" **[VERIFIED]**
(<https://ares-developers.github.io/Ares-docs/notes.html>). Syringe's `SyringeHandshake` /
`.syexe00` mechanism is the machinery that implements this — exe CRC32, size and timestamp are all
handed to the DLL, which can veto the launch **[VERIFIED]** (Syringe source). The known failure mode
is instructive: when Steam and Origin shipped slightly different `gamemd.exe` builds, Ares' check
rejected them and a Syringe hotfix was needed to bypass it **[CLAIMED]**
(<https://steamcommunity.com/sharedfiles/filedetails/?id=3203557419>). **Design the version check so
it can be updated without a full release.**

### Data-driven extension

Ares established, and Phobos follows, an **extension-object side table keyed off each vanilla type
object, populated by hooking the vanilla INI loader** — detailed under *Making patches data-driven*
below. The payoff: a modder writes a new key in an INI section they already understand and gets new
engine behaviour. **No modder ever sees an address.** That single decision is why Phobos has 42
contributors and thousands of downstream mods.

---

## StarCraft: Brood War plugin loaders

Brood War (1998) is the closest analogue to TA in age, and its ecosystem contributes four mechanisms
the C&C stack does not.

**The loader.** BWAPI's own quick-start is the whole architecture: install Brood War, **update to
`1.16.1`**, build an AI as a DLL, copy it to `bwapi-data/AI` inside the StarCraft folder, then "run
StarCraft through **Chaoslauncher**", tick *"BWAPI Injector x.x.x [RELEASE]"*, and click Start
**[VERIFIED]** (<https://github.com/bwapi/bwapi/blob/master/README.md>). So Chaoslauncher is a
third-party launcher with a checklist of plugins, one of which is the BWAPI injector; BWAPI itself
is a DLL that ends up inside `StarCraft.exe`. Its `DllMain` calls `CheckVersion()`, then
`ApplyCodePatches()`, then spawns a "BWAPI Persistent Patch" thread that re-applies its hooks every
300 ms **[VERIFIED]**
(<https://github.com/bwapi/bwapi/blob/master/bwapi/BWAPI/Source/DLLMain.cpp>). **I could not
determine Chaoslauncher's own plugin ABI or find its source** — it is not in the BWAPI repo, and I
did not locate an authoritative description. Treat the injection primitive it uses as undetermined.

**Mechanism 1 — bind C++ names directly to fixed addresses.** `bwapi/BWAPI/Source/BW/Offsets.h`
defines `#define IS_REF(name,addr) (& name) = *reinterpret_cast<…>(addr);` and then declares the
entire game state as references into `StarCraft.exe`'s memory **[VERIFIED]**
(<https://github.com/bwapi/bwapi/blob/master/bwapi/BWAPI/Source/BW/Offsets.h>): e.g.
`std::array<PlayerInfo, PLAYER_COUNT> IS_REF(Players, 0x0057EEE0);`,
`CUnit* IS_REF(UnitNodeList_VisibleUnit_First, 0x00628430);`, and functions as typed pointers —
`static void(*const BWFXN_sendTurn)() = (void(*)()) 0x00485A40;`, `BWFXN_QueueCommand = 0x00485BD0`.
It is the same idea as YRpp's `JMP_THIS`, expressed as data rather than as method bodies, and it is
the artifact that made an entire academic AI-competition ecosystem possible.

**Mechanism 2 — hook the game's own callback table instead of patching code.** BWAPI draws overlays
by overwriting function pointers in StarCraft's existing screen-layer array:
`BW::BWDATA::ScreenLayers[5].pUpdate = DrawHook;` and `ScreenLayers[2].pUpdate = DrawDialogHook;`,
re-asserted by the persistent-patch thread **[VERIFIED]** (`DLLMain.cpp`, `Offsets.h` —
`std::array<layer, 8> IS_REF(ScreenLayers, 0x006CEF50)`). **Where a 90s engine already dispatches
through a function-pointer table, replacing an entry is strictly better than a trampoline**: no
instruction-boundary risk, no overlapping-hook conflicts, trivially reversible.

**Mechanism 3 — the lockstep-safe way to act on the game.** `Offsets.h` exposes the command queue
itself — `std::array<u8, TURN_BUFFER_SIZE> IS_REF(TurnBuffer, 0x00654880);`,
`u32 IS_REF(sgdwBytesInCmdQueue, 0x00654AA0);` — alongside `BWFXN_QueueCommand` and
`BWFXN_sendTurn` **[VERIFIED]**. BWAPI issues orders **into the same per-turn command stream the
game already replicates to peers**, rather than mutating simulation state directly. This is the
single most important transferable idea in this document for a lockstep RTS: *inject orders, not
state.*

**Version gating.** A third technique, distinct from Syringe's CRC32 and Vinifera's MD5: BWAPI reads
the executable's own PE **VERSIONINFO** resource via `GetCurrentProductVersion()` and compares
against compile-time `SC_VER_1/2/3` constants; on mismatch it warns that it "will attempt to
continue to run in a reduced functionality mode" rather than refusing outright **[VERIFIED]**
(`DLLMain.cpp`). Note the softer posture — degrade, don't abort — which suits a research tool but
would be wrong for a multiplayer sim extension.

**Multiplayer/desync posture.** BWAPI's stated defaults are about *information*, not netcode: it
"only reveals the visible parts of the game state to AI modules by default… enabling programmers to
write competitive non-cheating AIs", and denies user input by default, with a Tournament Module able
to enforce those defaults **[VERIFIED]** (README). Its two-client self-play instructions run both
sides on one machine via `Chaoslauncher - MultiInstance.exe` **[VERIFIED]** (README). **I found no
documented checksum or version-match enforcement for BWAPI in ladder/online play, and no statement
about plugins being restricted to local use — undetermined.**

**Distribution & survival.** Source on GitHub, binaries via GitHub Releases; nothing Blizzard-owned
is shipped — the user must install and patch their own Brood War to 1.16.1 — with an explicit
trademark notice **[VERIFIED]** (README). The project is pinned to the pre-Remastered `1.16.1`
build, which is how it has stayed stable for a decade.

**Mechanism 4 — stop hardcoding addresses; *find* them by static analysis.** This is the most
important idea in the Brood War ecosystem and the answer to the moving-target-exe problem. BWAPI's
hardcoded `Offsets.h` is why it never crossed the 1.16.1 → Remastered boundary: issue
[#673](https://github.com/bwapi/bwapi/issues/673) is the explicit checklist of what broke —
`SDrawRealizePalette` no longer exported by `Storm.dll`, need for "support for a dynamic base address
for offsets", dozens of offsets to re-find — and it was never merged **[VERIFIED via subagent]**.
AIIDE 2026 and CoG 2026 still mandate Brood War 1.16.1 with BWAPI 4.x, nine years after 1.18
shipped **[VERIFIED via subagent]**.

The Remastered-era answer, by neivv (Markus Heikkinen), is a toolchain that **derives** addresses
instead of storing them **[VERIFIED — repos and manifests read directly]**:

- `neivv/scarf` (v0.3.0) — an x86 code-analysis library.
- `neivv/samase_scarf` — uses scarf to locate a given build's functions and globals by *analysing the
  binary*, with a `dump` binary for offline extraction (<https://github.com/neivv/samase_scarf>).
- `neivv/whack` (v0.2.0) — the in-process hooking library that applies the patches.
- `neivv/samase_plugin` (v0.5.0) — the plugin ABI, and the key design: `pub const VERSION: u16 = 44;`
  plus `pub enum FuncId` / `pub enum VarId`, with entry points such as `hook_step_objects`,
  `hook_process_commands` and `write_exe_memory` **[VERIFIED]**
  (<https://github.com/neivv/samase_plugin/blob/master/src/lib.rs>). **Plugins name *what* they want
  by symbolic id; the loader resolves *where* it lives in this particular build.** That indirection
  is what survives a game patch — and it is exactly what Syringe's raw `DEFINE_HOOK(0x423B95, …)`
  cannot do.
- `neivv/samase` itself **404s** — it is distributed as a forum attachment, so its hooking internals
  are **not verifiable from source** **[VERIFIED that the repo is absent]**.

Independent corroboration that this is the durable design: `ShieldBattery/ShieldBattery` (MIT, a
custom server and launcher for retail StarCraft: Remastered, "utilizing the real game client for
gameplay", actively developed) depends on the same two crates — `samase_scarf` for analysis and
`whack` for hooking — and reports version failures as "StarCraft build {build} is not supported"
rather than shipping a fixed offset table **[VERIFIED for the repo and its description]**
(<https://github.com/ShieldBattery/ShieldBattery>); the crate-dependency detail is **[VERIFIED via
subagent]**.

*(Diablo II loaders such as PlugY were not verified in the time available — mechanism undetermined,
omitted rather than guessed.)*

## FAF and other precedents

### FAF — the third mechanism: decompile-to-object and *re-link* the exe

Forged Alliance Forever does not inject and does not hex-edit. It **converts the PE back into a COFF
object, adds new code as new PE sections, and re-links the whole executable with GNU ld.**

- **Patches.** `FAForever/FA-Binary-Patches` (MIT, created 2020-06-28, last push 2026-08-16, forked
  from `kyx0r/FA_Patcher`) holds "documentation about the executable and various binary patches
  written in assembly and/or C++" **[VERIFIED]**
  (<https://github.com/FAForever/FA-Binary-Patches>). Two kinds: **hooks** (~106 files under
  `hooks/`) placing a function at a fixed address via inline asm — `hooks/DesyncFix.cpp` opens
  `".section h0; .set h0,0x6E4150;"` — and **signature patches** (`SigPatches.txt`), byte
  search-and-replace with wildcards and occurrence counts (one inlines a hot Lua-state accessor at
  792 call sites). `Info.txt` is the reverse-engineered offset map; `contributing.md` requires x86
  asm, MSVC C++, and IDA / x32dbg / Cheat Engine **[VERIFIED]**.
- **Toolchain.** `FAForever/FA_Patcher` (portable-executable-library, asmjit, TinyCC), superseded by
  `FAForever/fa-python-binary-patcher` (GNU ld 2.40; SigPatches applied post-build) **[VERIFIED]**.
  The *generic* version is **`FunkyFr3sh/petool`**, "Tool to help rebuild and patch 32-bit Windows
  applications": `pe2obj` (PE → win32 object), `genlds` (ld script for re-linking), `genmak`,
  `genprj`, `genpatch` (diff two exes → patch macros), `import` (import table as assembly), and
  **`genproxy` (generate a proxy-DLL project)**; patches are written in ordinary C/C++ with macros
  from `inc/macros/patch.h` **[VERIFIED]** (<https://github.com/FunkyFr3sh/petool>). **This is an
  off-the-shelf implementation of the entire static-patch playbook, and it would work on
  `TotalA.exe`.**
- **Version enforcement — not an exe hash.** The patched exe is just another versioned file in a
  per-featured-mod manifest: `patch_info.json` lists `ForgedAlliance.{}.exe` beside the `.nxt`
  archives, and the API serves `url, name, group, version, md5` per file, checked before install
  **[VERIFIED for manifest shape]** (<https://github.com/FAForever/fa/blob/develop/patch_info.json>,
  <https://github.com/FAForever/downlords-faf-client/issues/1352>). A server-side reject-on-mismatch
  gate was **not found — undetermined**. Runtime integrity is a per-beat simulation checksum →
  desync detection **[CLAIMED]**, corroborated by `hooks/DesyncFix.cpp` existing **[VERIFIED]**; the
  sim "happens entirely on the player's machines, and NOT on any server" **[VERIFIED]**
  (<https://github.com/FAForever/server>), with NAT traversal by `FAForever/java-ice-adapter`.
- **Distribution — the model changed, and the reason is instructive.** Originally
  `FAForever/binary-patch` (2014) shipped *only diffs*: `bsdiff4.diff(source, target)`, each patch
  named by `md5(source_data)`, with separate `retail.json`/`steam.json` — no copyrighted bytes
  redistributed **[VERIFIED]** (<https://github.com/FAForever/binary-patch>, archived 2020-06-29).
  Today FAF ships the built exe but **gates it on proven ownership**: "we can at no point upload the
  executable as an artifact… Before a user can use such end points his or her account needs to be
  verified" — by linking Steam or GOG **[VERIFIED]** (FA-Binary-Patches README). FA-Binary-Patches
  was created one day before binary-patch was archived.
- **What actually needed exe patching** (everything else is Lua): 4 GB address space, max sim rate
  50, replay-desync-on-player-leave fix, intel update every tick instead of every 30, custom world
  rendering primitives, an Ultra preset, new unit categories (`CANLANDONWATER`,
  `OBSTRUCTSBUILDING`), camera performance, extra mouse bindings **[VERIFIED]** (`changelog.md`).
  Unit cap and resolution: **no evidence found — undetermined.**
- **Alive**: release 3839 on 2026-08-28; `fa` pushed 2026-08-30 **[VERIFIED]**. Volunteer-run,
  donation-funded **[CLAIMED]**; team size not determinable.

### Total Annihilation's own precedent — validate the playbook against it

`FunkyFr3sh/Total-Annihilation-Patch-Loader` was "made to replace the hex edited TotalA.exe in the
Total Annihilation community patch" — the package ships with **no `TotalA.exe` at all**
**[VERIFIED]** (<https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader>). Read from
`dllmain.c` and `Makefile` **[VERIFIED]**: the artifact is a **`dplayx.dll` proxy** that forwards
every export on to `tplayx.dll` (`#pragma comment(linker, "/export:DirectPlayCreate=tplayx.DirectPlayCreate,@1")`
and eight more), refuses to run without official 3.1 ("Game version not supported. Please install
the official 3.1 patch"), `LoadLibrary`s `tdraw.dll` and rewrites the two `DirectDrawCreate` call
sites at `0x0047BFA2` and `0x004B55FB` via `patch_call()`, and signals `tdraw.dll` that the proxy is
active by setting a byte in a code cave. It is built with `i686-w64-mingw32-gcc`. Its byte patches
are declared in `res/patches.ini`, embedded in the DLL's resources, each with the file offset and VA
in a comment — `RegistryPath`, `ConfigFileName`, `Gp3FileName`, `GameVersionString`,
`ChangePathfindingSearch`, `CommanderOrdersNotReset`, `DirectxPopupElimination`,
`IncreaseAtmToFillResources`, `EnableF10Debug` **[VERIFIED]**. Sibling `res/prota.ini` carries
"ProTA 4.5 Patches" with original-vs-new opcodes documented in comments **[VERIFIED]**.

Critically, `patches.ini` exposes **`MultiplayerVersionMajor` / `MultiplayerVersionMinor`** with the
comment *"Change version # in multiplayer battleroom (all players must match)"* **[VERIFIED]** — TA
already has an in-protocol version field, which is the natural hook for lockstep enforcement.

Two more TA data points **[VERIFIED via subagent source reads]**: `jchristi/tademo99b2-src` (TA Demo
Recorder 0.99b2, source released 2003) is the same architecture — a `dplayx.dll` proxy for
recording, a `ddraw.dll` proxy carrying `tahook.cpp`, and a `VisPatcher/` that patches
`totala.exe`'s import table to load `spank.dll` instead of `ddraw.dll` *only as a Win95 fallback*.
And `ta-forever/gpgnet4ta` (MIT) does networking entirely **out of process** — it answers DirectPlay
enumeration on TCP 47624 and presents remote players as LAN peers, tunnelling through one UDP port
for ICE, using only `OpenFileMapping`/`MapViewOfFile`, with **no `WriteProcessMemory`, no injection,
no exe patching at all** (<https://github.com/ta-forever/gpgnet4ta>).

### Vinifera — the same playbook, on a 1999 binary, still shipping in 2026

The single best proof that the Syringe model generalises to a genuinely 90s RTS.
**Vinifera** is "an open-source community collaboration project extending the Tiberian Sun engine",
originally created by **CCHyper** and **tomsons26**, GPL-3.0, repo created 2021-03-30 and pushed to
**2026-08-29** **[VERIFIED]** (<https://github.com/Vinifera-Developers/Vinifera>, GitHub API).

- **Mechanism**: it uses its own fork, `Vinifera-Developers/SyringeEx` (LGPLv3), "for injection into
  the game's executable", and the user launches `LaunchVinifera.exe` **[VERIFIED]** (README).
- **Version pinning is brutally explicit**: "This project currently only supports the latest English
  (US) version of Tiberian Sun due to technical limitations with patching the original binary —
  `GAME.EXE; v2.03[EN]; Monday 5th June, 2000`, `MD5: C2C58CBBF83AF0458DC44EF64A3C011F`"
  **[VERIFIED]** (README). One binary, one MD5, documented up front. This is the honest position and
  TA should copy it.
- **Data-driven from the start**: a `VINIFERA.INI` with `ProjectName`, `IconFile`, `CursorFile`,
  `SearchPaths=INI,MIX` — i.e. the extension immediately gives mods a rebranding/asset-path API
  **[VERIFIED]** (README).
- **Distribution/legal**: nightly builds via GitHub Actions plus stable releases; "No assets, texts,
  artwork or other media from the original game(s) is included in this repository"; "EA has not
  endorsed and does not support this product" **[VERIFIED]** (README).
- **Client integration**: for the TS Client (CnCNet's launcher), the instructions replace `Game.exe`
  with a build from `CnCNet/ts-patches` and point `GameExecutableNames` at `LaunchVinifera.dat`
  **[VERIFIED]** (README, <https://github.com/CnCNet/ts-patches>) — showing how an injection
  project and a matchmaking client are wired together in practice.

### Source-release contrast (why C&C is not a fair comparison, and TA is)

EA has published source for **Tiberian Dawn**, **Red Alert**, **Renegade** and **Generals/Zero
Hour** (repos created 2024-08-22), plus the Remastered Collection (2020-03-30) and
`CnC_Modding_Support` (2025) **[VERIFIED]** (GitHub API, <https://github.com/electronicarts>).
There is **no** equivalent engine-source release for Tiberian Sun or Red Alert 2 — which is exactly
why Ares/Phobos/Vinifera exist as *binary* extensions while RA1/TD modding moved to Vanilla Conquer
/ OpenRA-style forks. **TA is in the TS/RA2 bucket: no source, so binary extension is the only
route.**

## Cross-cutting mechanisms (comparison table)

| Project | Target binary | Mechanism | Source open? | Data-driven API for modders | Alive 2026? |
|---|---|---|---|---|---|
| **AoE2 UserPatch** | `age2_x1.exe` (1999) | **Static byte patch** applied by `SetupAoC.exe` to the user's own file, optionally producing `age2_x1.5.exe`; ships `wndmode.dll`/`miniupnpc.dll` alongside **[VERIFIED]** | **No** | Yes — 50+ AI script commands, Strategic Numbers, RMS commands **[VERIFIED]** | v1.5 RC frozen at 2019; ecosystem alive |
| **aoc-mmmod / withmorten dsound** | `age2_x1.exe` | **Proxy DLL** impersonating `language_x1_p1.dll` / `dsound.dll`, forwarding to the real one **[VERIFIED]** | Yes | `mmm_*` DLL lifecycle API **[VERIFIED]** | Yes |
| **RA2 Syringe / SyringeEx** | `gamemd.exe` (2001) | **Debugger-based loader**: starts process, breaks at entry point, writes `E9` trampolines in memory, detaches; exe on disk untouched **[VERIFIED]** | Yes (LGPLv3) | n/a (it's the injector) | Yes (SyringeEx active) |
| **Ares** | `gamemd.exe` 1.001 | Hook DLL over Syringe **[VERIFIED]** | Yes | Hundreds of new INI tags **[VERIFIED]** | Maintained by one dev (AlexB); pushes through 2025 |
| **Phobos** | `gamemd.exe` 1.001 | Hook DLL over SyringeEx **[VERIFIED]** | Yes (GPL-3.0) | New INI tags via ext-class pattern **[VERIFIED]** | Yes, 42 contributors, active Aug 2026 |
| **CnCNet yrpp-spawner** | `gamemd.exe` | Hook DLL over Syringe; replaces lobby/netcode; `spawn.ini` **[VERIFIED]** | Yes (GPL-3.0) | `spawn.ini` | Yes |
| **BWAPI** | `StarCraft.exe` 1.16.1 | DLL loaded via Chaoslauncher's "BWAPI Injector" plugin; `DllMain` → `ApplyCodePatches()` + a 300 ms re-patch thread; overlays by **overwriting `ScreenLayers[].pUpdate` function pointers** **[VERIFIED]** | Yes | AI modules are DLLs in `bwapi-data/AI`; orders go into the game's own command queue | Yes; pinned to pre-Remastered 1.16.1 |
| **Vinifera** | TS `GAME.EXE` v2.03, MD5-pinned | Own SyringeEx fork + `LaunchVinifera.exe` **[VERIFIED]** | Yes (GPL-3.0) | `VINIFERA.INI` | Yes, pushed 2026-08-29 |
| **FAF** | `ForgedAlliance.exe` | **PE → COFF object → new sections → re-link with GNU ld**; plus wildcard signature patches **[VERIFIED]** | Yes (MIT) | Game logic is Lua; exe patches are engine-level | Yes, release 3839 on 2026-08-28 |
| **petool** *(generic)* | any 32-bit PE | `pe2obj`/`genlds`/`genmak`/`genpatch`/`genproxy` toolchain **[VERIFIED]** | Yes | patch macros in C/C++ | Yes |
| **TA Patch Loader** | `TotalA.exe` 3.1 | **`dplayx.dll` proxy** forwarding to `tplayx.dll`, applying byte patches from an INI in its resources **[VERIFIED]** | Yes | `patches.ini` / `prota.ini` | Yes |
| **TA Forever `gpgnet4ta`** | none | **Out-of-process** DirectPlay proxy on TCP 47624; no injection, no patching **[VERIFIED]** | Yes (MIT) | n/a | Yes |

## Handling multiplayer version lockstep

These games are **lockstep-deterministic**: every client simulates the same frames from the same
orders, so any divergence in simulation code desyncs the game. (The canonical description of the
AoE model is Mark Terrano & Paul Bettner's *"1500 Archers on a 28.8: Network Programming in Age of
Empires and Beyond"*, catalogued at <https://github.com/SiegeEngineers/aoc-dev-resources>
**[VERIFIED]**.) Three distinct strategies appear:

1. **Classify features by sync-impact and tell the user.** UserPatch's `(sync)` marker: features so
   tagged must be uniformly installed or uninstalled across every player **[VERIFIED]**
   (<https://userpatch.aiscripters.net/guide.html>). Cheap; relies on discipline.
2. **Refuse to run on an unrecognised binary.** Syringe hands each hook DLL the target's file size,
   PE timestamp and CRC32 via `SyringeHandshake`, and independently checks a `.syexe00` host
   declaration; the DLL returns `S_OK` or the injection is vetoed **[VERIFIED]** (Syringe source).
   Ares uses this to refuse third-party-patched executables, since its hooks are address-specific
   **[VERIFIED]** (<https://ares-developers.github.io/Ares-docs/notes.html>). Failure mode:
   Steam/Origin reissues of `gamemd.exe` broke the check and needed a bypass hotfix **[CLAIMED]**
   (<https://steamcommunity.com/sharedfiles/filedetails/?id=3203557419>). Build the check so it can
   be updated out-of-band.
3. **Push enforcement to the lobby service.** Voobly distributes UserPatch as a managed "game mod"
   with its own patch layer and anti-cheat **[CLAIMED]**
   (<https://www.voobly.com/gamemods/mod/51/UserPatch>). CnCNet does the equivalent by owning the
   launch path: the client writes `spawn.ini` and starts the game through the spawner DLL, which
   itself contains "event verification checks" and desync fixes **[VERIFIED]**
   (<https://github.com/CnCNet/yrpp-spawner>). **This is the only approach that actually enforces
   anything** — whoever controls matchmaking controls the version.

4. **Treat the patched exe as just another versioned file in a manifest.** FAF's approach: the
   patched `ForgedAlliance.{}.exe` sits in `patch_info.json` alongside the `.nxt` archives, and the
   API serves a per-featured-mod file list with an `md5` for each entry, verified before install
   **[VERIFIED for manifest shape]** (<https://github.com/FAForever/fa/blob/develop/patch_info.json>,
   <https://github.com/FAForever/downlords-faf-client/issues/1352>). I could **not** verify a
   server-side reject-on-mismatch gate. What FAF actually relies on at runtime is a per-beat
   **simulation checksum** whose mismatch is reported as a desync **[CLAIMED]**, corroborated by
   `hooks/DesyncFix.cpp` existing **[VERIFIED]**.

Three structural tricks worth stealing:

- **Inject orders, not state.** BWAPI does not mutate the simulation; it writes into StarCraft's own
  per-turn command queue (`TurnBuffer` at `0x00654880`, `sgdwBytesInCmdQueue` at `0x00654AA0`) and
  calls the game's `QueueCommand`/`sendTurn` **[VERIFIED]**
  (<https://github.com/bwapi/bwapi/blob/master/bwapi/BWAPI/Source/BW/Offsets.h>). Anything that
  enters the replicated order stream is desync-safe by construction; anything that writes sim state
  on one machine is not.

- **Keep the wire format byte-identical.** UserPatch added spectator IPs and feature flags by
  carving 20 bytes out of an existing 260-byte filename field rather than extending the lobby struct
  **[VERIFIED]** (<https://github.com/SiegeEngineers/userpatch-notes/blob/master/notes/multiplayer.md>).
- **Use the game's own version field.** TA already carries a multiplayer version number that all
  players must match — the TA Patch Loader exposes it as `MultiplayerVersionMajor` /
  `MultiplayerVersionMinor` with the comment *"Change version # in multiplayer battleroom (all
  players must match)"* **[VERIFIED]**
  (<https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader/blob/main/res/patches.ini>). An
  extension that bumps this gets crude but free lobby-level segregation from unpatched clients.

## Making patches data-driven for modders

This is the difference between a patch and a platform.

- **Ares/Phobos: extension objects driven by the game's own INI parser.** For every vanilla type
  class there is an `…Ext` class holding new fields as `Valueable<T>` / `Nullable<T>` /
  `ValueableVector<T>`; `LoadFromINIFile()` reads them by key from the type's existing INI section;
  and **one** hook on the game's type-loading routine
  (`DEFINE_HOOK(0x4287DC, AnimTypeClass_LoadFromINI, 0xA)`) dispatches into the ext map
  **[VERIFIED]** (<https://github.com/Phobos-developers/Phobos/blob/develop/src/Ext/AnimType/Body.cpp>).
  Adding a modder-facing feature is then: declare a field, read it, use it in one behaviour hook.
- **UserPatch: extend the script languages.** 50+ new AI commands, new Strategic Numbers, new random
  map script commands — a text API, documented and searchable **[VERIFIED]**
  (<https://userpatch.aiscripters.net/reference.html>, <https://airef.github.io/>).
- **Ship a "pp" header library.** `YRpp` "provides the necessary headers to interact with Yuri's
  Revenge's binary and data types in C++" **[VERIFIED]**
  (<https://github.com/Ares-Developers/YRpp>). Two things in one: (a) C++ structs whose field
  layout matches the game's in memory, and (b) methods that call the game's own code — e.g.
  `void AddPassenger(FootClass* p) { JMP_THIS(0x4733A0); }`, where `JMP_THIS` tears down the MSVC
  thiscall frame and jumps to the game's function at that address **[VERIFIED]**
  (`TechnoClass.h`, `ASMMacros.h`). This artifact is what lets a contributor write ordinary C++
  against a 25-year-old binary.
- **Keep a registry of hooked addresses.** Because Ares and Phobos hook the same process, Phobos
  ships tooling that cross-checks every new `DEFINE_HOOK` against Ares' hook list for address
  conflicts, hooks smaller than the 5 bytes an `E9 rel32` needs, instruction-boundary
  misalignment, and relative instructions inside the overwritten range **[VERIFIED]**
  (<https://github.com/Phobos-developers/Phobos/blob/develop/.agents/skills/check-hooks/SKILL.md>).
  SyringeEx solves the last of these in the loader instead, advertising a `ReladdrInstructionFixup`
  feature flag — relative `JMP`/`CALL`/`Jcc` in overwritten code are relocated (it vendors Zydis to
  disassemble them), alongside `ESPModification` and `ZFPreservation` flags **[VERIFIED]**
  (<https://github.com/Phobos-developers/SyringeEx>).
- **Prefer callback-table replacement to code patching where the engine offers it.** BWAPI installs
  its renderer by assigning `BW::BWDATA::ScreenLayers[5].pUpdate = DrawHook;` — an entry in a
  function-pointer table the game already dispatches through — and re-asserts it from a 300 ms
  watchdog thread **[VERIFIED]**
  (<https://github.com/bwapi/bwapi/blob/master/bwapi/BWAPI/Source/DLLMain.cpp>). No trampolines, no
  instruction-boundary hazard, no conflict with other extensions.
- **Zero-configuration plugin discovery.** Syringe scans `*.dll` in the game folder and loads
  anything carrying a `.syhks00` section **[VERIFIED]** (Syringe source), which is why Phobos'
  install instructions are literally "drop the files in the game folder; Syringe will load Phobos
  automatically" **[VERIFIED]** (<https://github.com/Phobos-developers/Phobos>).

## Distribution & legal handling

The uniform pattern: **never redistribute the game binary; ship only your own artifacts and apply
them to the user's own copy.**

- UserPatch ships an installer requiring "a valid, existing installation of Conquerors v1.0c" and
  explicitly does not provide a no-CD **[VERIFIED]** (<https://userpatch.aiscripters.net/guide.html>).
  Its installer takes per-feature flags (`SetupAoC.exe -i:FLAGS`) so the same distributable produces
  many configurations **[VERIFIED]**
  (<https://github.com/SiegeEngineers/userpatch-notes/blob/master/notes/installation.md>).
- Syringe/Ares/Phobos/yrpp-spawner ship a loader and DLLs only — the game exe is never touched on
  disk, which sidesteps the derivative-work question about distributing a modified binary
  **[VERIFIED]** (<https://github.com/Ares-Developers/Syringe>,
  <https://phobos.readthedocs.io/en/latest/>). Licences are LGPLv3 (Syringe/SyringeEx) and GPL-3.0
  (Phobos, yrpp-spawner) **[VERIFIED]**. Both carry explicit disclaimers — "EA has not endorsed and
  does not support this product"; "an unofficial open-source community collaboration project"
  **[VERIFIED]** (<https://github.com/Phobos-developers/Phobos>,
  <https://github.com/CnCNet/yrpp-spawner>).
- Ares is distributed from Launchpad, Phobos from GitHub Releases with nightly builds via GitHub
  Actions **[VERIFIED]** (<https://github.com/Phobos-developers/Phobos>).
- **FAF shows both answers to the hardest case — you genuinely need to ship a modified exe.**
  First model (2014–2020): distribute a **bsdiff** only, named by the MD5 of the *source* file, with
  separate descriptors for retail and Steam installs, so the patch is meaningless without the user's
  own copy **[VERIFIED]** (<https://github.com/FAForever/binary-patch>, archived 2020-06-29). Second
  model (2020–): ship the built exe, but only behind **account ownership verification** via a linked
  Steam or GOG account — "we can at no point upload the executable as an artifact… Before a user can
  use such end points his or her account needs to be verified" **[VERIFIED]**
  (<https://github.com/FAForever/FA-Binary-Patches>).
- **TA's own community moved the other way, and that is the most relevant precedent.** The
  `Total-Annihilation-Patch-Loader` exists explicitly to "replace the hex edited TotalA.exe in the
  Total Annihilation community patch", so the shipped package contains **no `TotalA.exe`**
  **[VERIFIED]** — trading a redistribution problem for a DLL-search-order trick.

**The one hard legal precedent anyone in this space cites.** *Davidson & Associates v. Jung* (the
**bnetd** case), 8th Circuit 2005 — a clean-room Battle.net server reimplementation was held to
violate the EULA's reverse-engineering ban and DMCA §1201, and **the modders lost** **[VERIFIED via
subagent]** (<https://www.eff.org/cases/blizzard-v-bnetd>). Note what it was actually about:
reimplementing an *online service* and circumventing its authentication, not patching a
single-player binary. Its successor PvPGN is nonetheless developed openly today. The practical
lesson for TA is narrow but real — **modifying the game you own is a very different posture from
re-implementing or circumventing a live service**, and the projects that survived stayed on the
first side of that line.

**Realism check on effort.** Ares: conceived 2007, public 2010, repo since 2011, ~22–24 credited
people across its life, but a succession of *single* lead developers (pd → DCoder/Renegade/Electro/
Marshall/Graion Dilach → AlexB today) **[VERIFIED]**
(<https://ares-developers.github.io/Ares-docs/credits.html>, GitHub API). Phobos: started 2020,
42 GitHub contributors, but its own README currently warns "the project is currently not maintained
actively enough and thus we are looking for active maintainers" **[VERIFIED]**. UserPatch: roughly
2009–2019, effectively one author, source never released. **Expect a bus factor of one for the
first several years.**

## The playbook for TA

*(Cross-reference: `exe-reverse-engineering.md` for what is known about `TotalA.exe` itself, and
`runtime-injection.md` for TA-specific loader/hooking practicalities. This section is only the
transferable pattern.)*

1. **Do not ship a patched `TotalA.exe`. Patch in memory at load time.** This is not a theoretical
   recommendation — TA's own community already made this exact migration, and
   `FunkyFr3sh/Total-Annihilation-Patch-Loader` is a working, readable reference implementation
   against the exact binary you care about **[VERIFIED]**. The static-patch route couples you to one
   build, produces an artifact you cannot legally redistribute, and — see UserPatch — has to be
   reverse-engineered by your own successors.
2. **Start from the proxy-DLL pattern, and specifically from the existing TA one.** TA already
   loads `dplayx.dll` and `ddraw.dll` from its own directory, and both slots are in active community
   use (`dplayx.dll` → TA Patch Loader and TA Demo Recorder; `ddraw.dll`/`tdraw.dll` →
   the interface upgrade). The `Total-Annihilation-Patch-Loader` `dllmain.c` shows the whole trick in
   ~110 lines: forward every export with
   `#pragma comment(linker, "/export:DirectPlayCreate=tplayx.DirectPlayCreate,@1")`, gate on the 3.1
   signature, then patch. **Caveat to check first**: Windows resolves KnownDLLs before the
   application directory, so a KnownDLL cannot be shadowed locally; `ddraw.dll` is not on the stock
   Win10/11 KnownDLLs list — which is why `cnc-ddraw` works — but DDrawCompat's install guide still
   warns "the game may still use only the system ddraw.dll" and lists workarounds **[VERIFIED]**
   (<https://github.com/narzoul/DDrawCompat/wiki/Installation-guide>). Fall back to a launcher
   (`CreateProcess` suspended + remote `LoadLibrary`) or Syringe's debugger-at-entry-point design
   only if the proxy slot proves unreliable.
3. **Do not hand-roll the toolchain.** `FunkyFr3sh/petool` already implements the generic version of
   all of this for any 32-bit PE: `pe2obj`, `genlds`, `genmak`, `genprj`, `genpatch` (diff two exes
   into patch macros — this imports every legacy TA byte patch for free), and **`genproxy`**
   (generate a proxy-DLL project) **[VERIFIED]** (<https://github.com/FunkyFr3sh/petool>). For
   runtime hooking do not write your own trampolines: **MinHook**, **PolyHook 2.0** or **Microsoft
   Detours**, all maintained and all 32-bit-capable in 2026, with **Zydis** for instruction-length
   decoding — exactly what SyringeEx does **[VERIFIED]**.
4. **Build the `TApp` header library before building features.** Reconstruct TA's core structs
   (unit instance, unit type/FBI-derived record, player, map cell, order queue) as C++ types with
   matching layout, plus bindings to the engine's own functions at their addresses. Two proven
   spellings: YRpp's `void AddPassenger(FootClass* p) { JMP_THIS(0x4733A0); }` (method bodies) and
   BWAPI's `IS_REF(Players, 0x0057EEE0)` / `BWFXN_sendTurn = (void(*)())0x00485A40` (references and
   typed pointers in one header) **[VERIFIED]**. This is the force multiplier: it converts "write
   assembly against a disassembly" into "write C++". Both the RA2 and Brood War ecosystems are
   downstream of this one artifact existing.
5. **Resolve addresses by analysis, not by constant — or at least keep the option open.** The
   difference between BWAPI (frozen on 1.16.1 forever, because `Offsets.h` is a wall of literals) and
   neivv's stack (still working across Remastered builds) is that the latter *derives* addresses by
   statically analysing the binary and exposes them to plugins as symbolic ids — `FuncId` / `VarId`,
   with a versioned ABI (`VERSION: u16 = 44`) **[VERIFIED]**. Full static analysis is overkill for a
   first release, but **do put an indirection layer between your feature code and the numbers**: a
   single `addresses.toml`/`Symbols` table that hook code refers to by name, never a literal inline
   in a hook. Retrofitting that later means touching every hook.
6. **Inject orders, not state — the single most important rule for a lockstep RTS.** Find TA's
   per-turn command/order queue and its enqueue + send-turn functions, and make that the primary API
   for anything that affects the simulation. BWAPI is the proof this works: it exposes `TurnBuffer`,
   `sgdwBytesInCmdQueue`, `BWFXN_QueueCommand` and `BWFXN_sendTurn` and drives an entire AI
   competition through them without desyncing **[VERIFIED]**. Anything that instead writes sim state
   directly on one machine will desync, and you will spend years chasing it.
7. **Make the very first shipped feature the data-driven plumbing, not a game feature.** Find TA's
   equivalent of `AnimTypeClass_LoadFromINI` — the routine that parses a unit's `.fbi`/TDF record —
   hook it once, and attach a per-type extension object populated from **new keys in the existing
   TDF sections**. Then every subsequent feature is a field plus a behaviour hook, and modders
   consume it by editing text files. If you only ship hardcoded features, the project dies with you.
8. **Maintain a machine-readable hook registry from commit #1.** A checked-in table of
   `{address, overridden_byte_count, symbol, owning_module}` plus a CI check for overlaps and
   sub-5-byte hooks. Phobos had to retrofit this against Ares; you can have it free. It is also what
   makes multiple independent extension DLLs possible later.
9. **Version-gate hard, but make the gate updatable.** Copy Syringe's handshake and Vinifera's
   honesty: hash the target exe (size + PE timestamp + CRC32) and refuse to run on an unrecognised
   build, with the accepted-hash list in a data file rather than compiled in. The TA Patch Loader
   already does the cheap version of this — a 10-byte signature check that aborts with "Please
   install the official 3.1 patch" **[VERIFIED]** — and Vinifera publishes a single supported MD5 in
   its README **[VERIFIED]**. Pin one build; document it; make widening the list a config change.
10. **Split features into `cosmetic` and `sync` from day one**, expose that split in the config, and
   put the real enforcement in the lobby. TA is lockstep, so a mismatched simulation hook desyncs.
   Two levers exist immediately: TA's own `MultiplayerVersion` field in the battleroom (bump it and
   unpatched clients cannot join) **[VERIFIED]**, and the out-of-process route — `ta-forever/gpgnet4ta`
   already owns DirectPlay enumeration and could refuse a lobby whose members report different
   extension hashes, without touching the exe at all **[VERIFIED]**. Long term the CnCNet model is
   the target: the matchmaking client owns the launch path and gates on an identical version +
   hook-set hash. Ship that handshake even in the first LAN-only release; retrofitting it is painful.
11. **Distribute a loader + DLL + docs. Never the exe.** GPL-3.0 or LGPLv3, GitHub Releases with
   nightly CI builds, an explicit "not endorsed by / not affiliated with the rightsholder"
   disclaimer, and a hard requirement that the user supplies their own installed copy.
12. **Plan for a bus factor of one.** Both flagship projects ran on a single lead for years. The
   things that let Phobos reach 42 contributors were, in order: an open repo, the `pp` header
   library, the one-macro hook declaration, and documentation aimed at modders rather than at
   reverse engineers. Build those four first.

## Sources

**AoE2 UserPatch**
- <https://userpatch.aiscripters.net/> — official site, feature list, version history
- <https://userpatch.aiscripters.net/guide.html> — install mechanism, `(sync)` marking
- <https://userpatch.aiscripters.net/reference.html> — AI / Strategic Number / RMS references
- <https://airef.github.io/> — community AI scripting encyclopedia
- <https://github.com/SiegeEngineers/userpatch-notes> — reverse-engineered notes (installer flags, `AGEPresetData`, scripter64 byte-patch quote)
- <https://github.com/SiegeEngineers/aoc-mmmod> — proxy-DLL mod loader, `mmm_*` API
- <https://github.com/withmorten/dsound> — `dsound.dll` proxy loader for `age2_x1`
- <https://github.com/SiegeEngineers/aoc-dev-resources> — RE resources incl. "1500 Archers on a 28.8"
- <https://www.voobly.com/gamemods/mod/51/UserPatch> — Voobly game-mod distribution *(403 to automated fetch; [CLAIMED])*

**RA2 / Syringe / Ares / Phobos**
- <https://github.com/Ares-Developers/Syringe> — injector source (`SyringeDebugger.cpp/.h`, `PortableExecutable.*`)
- <https://github.com/Ares-Developers/YRpp> — `Syringe.h` (`DEFINE_HOOK`, `REGISTERS`, `.syhks00`/`.syexe00`), `ASMMacros.h` (`JMP_THIS`), reconstructed game classes
- <https://github.com/Ares-Developers/Ares> — Ares source
- <https://ares-developers.github.io/Ares-docs/> — index, `notes.html` (compatibility), `credits.html` (team)
- <https://github.com/Phobos-developers/Phobos> — Phobos source, README (install, legal, releases)
- <https://github.com/Phobos-developers/Phobos/blob/develop/src/Ext/AnimType/Body.h> and `Body.cpp` — ext-class + INI pattern
- <https://github.com/Phobos-developers/Phobos/blob/develop/src/Ext/Anim/Hooks.cpp> — real `DEFINE_HOOK`/`DEFINE_PATCH` usage
- <https://github.com/Phobos-developers/Phobos/blob/develop/.agents/skills/check-hooks/SKILL.md> — hook-conflict tooling
- <https://github.com/Phobos-developers/SyringeEx> — feature flags, Zydis-based relative-instruction fixup
- <https://github.com/CnCNet/yrpp-spawner> — netcode/lobby layer as a Syringe DLL
- <https://steamcommunity.com/sharedfiles/filedetails/?id=3203557419> — Steam `gamemd.exe` version-check bypass *([CLAIMED])*

**Tiberian Sun / C&C source releases**
- <https://github.com/Vinifera-Developers/Vinifera> — TS engine extension (README: mechanism, MD5 pin, `VINIFERA.INI`, legal)
- <https://github.com/Vinifera-Developers/SyringeEx> — its injector fork
- <https://github.com/CnCNet/ts-patches> — patched `Game.exe` builds for the TS Client
- <https://github.com/electronicarts> — `CnC_Tiberian_Dawn`, `CnC_Red_Alert`, `CnC_Renegade`, `CnC_Generals_Zero_Hour` (2024-08-22), `CnC_Remastered_Collection` (2020-03-30), `CnC_Modding_Support` (2025)

**StarCraft: Brood War**
- <https://github.com/bwapi/bwapi/blob/master/README.md> — Chaoslauncher + "BWAPI Injector" workflow, 1.16.1 pinning, `bwapi-data/AI`, legal notice
- <https://github.com/bwapi/bwapi/blob/master/bwapi/BWAPI/Source/BW/Offsets.h> — `IS_REF` address bindings, command queue, `BWFXN_*` function pointers
- <https://github.com/bwapi/bwapi/blob/master/bwapi/BWAPI/Source/DLLMain.cpp> — `CheckVersion()` via PE VERSIONINFO, `ApplyCodePatches()`, `ScreenLayers[].pUpdate` hooks, persistent-patch thread

- <https://github.com/bwapi/bwapi/issues/673> — why BWAPI never crossed 1.16.1 → Remastered
- <https://github.com/neivv/scarf>, <https://github.com/neivv/samase_scarf>, <https://github.com/neivv/whack> — x86 analysis, address discovery, in-process hooking
- <https://github.com/neivv/samase_plugin/blob/master/src/lib.rs> — `VERSION`, `FuncId`/`VarId`, `hook_step_objects`, `write_exe_memory`
- <https://github.com/ShieldBattery/ShieldBattery> — independent SC:R client built on the same analysis+hooking crates
- <https://www.eff.org/cases/blizzard-v-bnetd> — *Davidson v. Jung* (bnetd), the governing legal precedent

**FAF**
- <https://github.com/FAForever/FA-Binary-Patches> — hooks (`.section h0; .set h0,0x…`), `SigPatches.txt`, `Info.txt`, `changelog.md`, ownership-verification statement
- <https://github.com/FAForever/FA_Patcher> and <https://github.com/FAForever/fa-python-binary-patcher> — the re-link toolchain
- <https://github.com/kyx0r/FA_Patcher> — upstream
- <https://github.com/FAForever/binary-patch> — the original bsdiff distribution model (archived 2020-06-29)
- <https://github.com/FAForever/fa/blob/develop/patch_info.json> — exe as a manifest entry
- <https://github.com/FAForever/downlords-faf-client/issues/1352> — file-manifest/md5 verification shape
- <https://github.com/FAForever/server>, <https://github.com/FAForever/java-ice-adapter> — P2P sim, NAT traversal

**Total Annihilation precedents**
- <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader> — `dplayx.dll` proxy, `dllmain.c`, `Makefile`, `res/patches.ini`, `res/prota.ini`
- <https://github.com/jchristi/tademo99b2-src> — TA Demo Recorder 0.99b2 source (dplayx + ddraw proxies, `VisPatcher` import-table patch)
- <https://github.com/ta-forever/gpgnet4ta> — out-of-process DirectPlay proxy
- <https://github.com/Skirmisher/TA-Patch-Installers> — inventory of legacy TA patches (unit-limit 500/1500/5000, pathfinding, sound, multicore)

**Tooling for a new project**
- <https://github.com/FunkyFr3sh/petool> — `pe2obj`, `genlds`, `genmak`, `genprj`, `genpatch`, `genproxy` for any 32-bit PE
- <https://github.com/FunkyFr3sh/cnc-ddraw> — DirectDraw reimplementation as a drop-in `ddraw.dll`
- <https://github.com/narzoul/DDrawCompat/wiki/Installation-guide> — KnownDLLs / DLL-search-order caveats for `ddraw.dll` proxies
- <https://github.com/TsudaKageyu/minhook> — minimal x86/x64 hooking library (active 2026)
- <https://github.com/stevemk14ebr/PolyHook_2_0> — C++20 hooking library
- <https://github.com/microsoft/Detours> — Microsoft's API-instrumentation package
- <https://github.com/zyantific/zydis> — disassembler used by SyringeEx for instruction-length decoding
