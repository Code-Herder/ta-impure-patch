# Candidates — Code-hosting / technical sweep

Scope: projects that add NEW ENGINE FEATURES to the original Cavedog `TotalA.exe` (1997) by
patching or hooking the closed-source binary. Engine re-implementations (Spring/Recoil, BAR,
Zero-K, TA3D, OpenTA, RWE, OpenRA, kbot) are explicitly OUT of scope and listed only as
context at the bottom.

Sweep date: 2026-08-31. Sources: GitHub REST/search API (authenticated), GitLab API,
SourceForge directory, general web search.

---

## Tier 1 — Actually patch/hook the 1997 binary

| Name | Author / org | Link | Lang | Last activity | What it does | Mechanism | Licence | Conf. |
|---|---|---|---|---|---|---|---|---|
| **TADR — TA Demo Recorder + TA Community Patch** | Axle1975 (lead), tanvanman, tagROCK, sc0tt88, dv-morais, FunkyFr3sh, zlatkok; forked from `svn.riouxsvn.com/tadr` (Rime, Xpoy, N72, Fnordia, SJ, Yeha) | https://github.com/tanvanman/TADR | C++ (`src/DDraw`) + Delphi/Pascal (`src/Recorder`, `src/Server`, `src/Launcher`) | **commit 2026-08-31**, tagged release `v2026.8.6` (2026-08-06), nightly `dev-*` builds | The single biggest live TA engine-feature project. Ships `tdraw.dll` + `tplayx.dll`. Adds ~100 engine features: raised unit/weapon/SFX/composite limits, megamap + fullscreen minimap, chat font/backdrop/routing, share guard, vote dialogs, auto-team, start positions, unit-def extensions, weapon TDF hooks, veterancy, reload bars, HUD notifications, unicode support, 10-player replay, bookmarks, build ghosts, commander warp, lag-switch guard, profiler, debug pipe server, plus dozens of engine bug fixes (area-damage overflow, weapon-ID overflow, off-map aircraft, grid-claim tie-break, repair rate, shading, transported explosions). Per-mod configs for OTA / ProTA / Escalation / Mayhem / BTA / TA Zero. | **All four at once:** (a) `ddraw.dll` proxy DLL with full AheadLib-style export forwarding (`src/DDraw/ddraw.def`); (b) `dplayx`→`tplayx.dll` DirectPlay proxy (`src/Recorder/tplayx.dpr`, `Dplayx_exports.pas`); (c) inline hooks / detours (`src/DDraw/hook/InlineHook.cpp`, `ModifyHook.cpp`, `GameTickHook.cpp`); (d) direct byte patching of the loaded exe at absolute VAs (`src/Recorder/InitCode_CoreExePatching.pas`, `HardCodeFunctions.cpp`, `HardCodedValue.cpp`) | **MIT** per component (tdraw.dll, tplayx.dll, SERVER.EXE each separately MIT; repo metadata says NOASSERTION because of the multi-block LICENSE) | **[VERIFIED]** |
| **Total Annihilation Patch Loader** | FunkyFr3sh | https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader | C | commit 2024-05-02; release `v1.3.0.0` (2023-10-12) | Replaces the hex-edited `TotalA.exe` shipped with the TA Community Patch so mod packages need not redistribute the exe. Applies the community-patch byte edits at runtime from an INI; users can add their own `address = hex` patches. | Builds as **`dplayx.dll`** (a DirectPlay proxy TA imports). In `DllMain` it fingerprints the exe (`memcmp` at base+0x10000 against a 10-byte signature, rejects non-3.1), then applies INI-driven absolute-VA byte patches with overlap/conflict detection (`patches.c`, `patch.h`, inih). | **MIT** | **[VERIFIED]** |
| **cnc-ddraw** | FunkyFr3sh / CnCNet | https://github.com/FunkyFr3sh/cnc-ddraw | C | **2026-08-30** (very active) | Not TA-specific, but the ddraw replacement the TA Unofficial Patch ships. Adds windowed/borderless, arbitrary resolution, upscaling shaders, Alt-Tab fix, CPU/perf fixes on Win10/11 and Wine. | **DirectDraw API wrapper/replacement** (`ddraw.dll` re-implemented over GDI/OpenGL/D3D9). Non-invasive to the exe. | Open source; SPDX not verified in this sweep | **[VERIFIED]** |
| **petool** | FunkyFr3sh | https://github.com/FunkyFr3sh/petool | C | 2024-12-28 | Generic tool to rebuild, extend and patch 32-bit Windows PE executables (add code sections to a closed-source exe). The obvious toolchain for adding new code to `TotalA.exe`; same author as the TA patch loader. | **Static PE patching / section injection** | Not verified (repo is public) | **[VERIFIED]** (TA use is [LEAD]) |
| **DxWnd / DxWnd.reloaded** | gho (SourceForge), GitHub mirror under `DxWnd` | https://github.com/DxWnd/DxWnd.reloaded (mirror, stale) · live: https://sourceforge.net/p/dxwnd/ | C/C++ | GitHub mirror last push **2017-04-22** (249★); SourceForge tree still maintained | Generic Windows hooker for old games. **Ships a `build/exports/Total Annihilation.dxw` profile.** Windowing, resolution/colour fixes, timing stretch, `dinput` hooking. Not a feature-adder for TA gameplay. | **IAT / hot-patch hooking** of `ddraw`, `dinput`, timing APIs, injected into the process | No SPDX licence on the GitHub mirror | **[VERIFIED]** |

## Tier 2 — Network/launcher layer around the unmodified exe (no binary patching)

| Name | Author / org | Link | Lang | Last activity | What it does | Mechanism | Licence | Conf. |
|---|---|---|---|---|---|---|---|---|
| **gpgnet4ta** | ta-forever (Axle1975) | https://github.com/ta-forever/gpgnet4ta | C++ / Qt / CMake | **2026-08-01** | The TAF launcher. Libs: `jdplay` (DirectPlay lobby launch + `DPlayWrapper`), `dplayreg` (writes the DirectPlay lobbyable-app registry keys), `tafnet` (tunnels all TA traffic through one UDP port for the ICE adapter), `tapacket` (TA/DirectPlay packet parser, TA demo reader/writer, `tadissector.lua` Wireshark dissector, `tasmartpak.lua`), `tareplay` (live replay/demo compiler client), `tafencrypt`, plus `apps/talauncher` and `apps/replayer`. | **Launcher + network-level interception.** Masquerades as local TA instances and proxies DirectPlay traffic; forges packet addresses (`GameAddressTranslater`). No `WriteProcessMemory` / `CreateRemoteThread` / `LoadLibrary` injection found in the tree. | **MIT** | **[VERIFIED]** |
| **downlords-taf-client** | ta-forever | https://github.com/ta-forever/downlords-taf-client | Java | **2026-08-23** | TAF lobby client (fork of FAForever's). `TotalAnnihilationService`, `GameService`, featured-mod install (`InstallFeaturedModTask`), `LocalDemoSealService` for replays. Launches `TotalA.exe`, installs the community-patch DLLs. | Launcher / installer | **MIT** | **[VERIFIED]** |
| **taftoolbox** | ta-forever | https://github.com/ta-forever/taftoolbox | C++ | 2026-07-24 | CLI tools for inspecting TA assets. | Asset tooling | GPL-3.0 | **[VERIFIED]** |
| **ta-forever/uid** | ta-forever (fork of FAForever/uid) | https://github.com/ta-forever/uid | C | 2025-04-23 | Collects an RSA-encrypted machine fingerprint to block smurfing. Not an engine feature. | Standalone exe run by the client | GPL-3.0 | **[VERIFIED]** |

Full `ta-forever` org (13 public repos, all enumerated): `downlords-taf-client`, `server`,
`gpgnet4ta`, `taftoolbox`, `api`, `db`, `moderator-client`, `taf-stack`, `java-ice-adapter`,
`uid`, `unrealircd`, `unrealircd-docker-only`, `trueskill-taf`. **Only `gpgnet4ta` touches the
game process at all, and it does so at the network/launcher layer, not by patching the binary.**

## Tier 3 — Distribution / installers of the patched game

| Name | Author | Link | Lang | Last activity | Notes | Licence | Conf. |
|---|---|---|---|---|---|---|---|
| TA-Unofficial-Patch-Install | gammata | https://github.com/gammata/TA-Unofficial-Patch-Install | (docs) | 2023-08-20, 54★ | Drop-in bundle of the TA Unofficial Patch 3.9.x + cnc-ddraw. Contains the built `tdraw.dll` / `tplayx.dll`, not their source. Most-starred TA patch repo. | none | **[VERIFIED]** |
| TA-Patch-Installers | Skirmisher | https://github.com/Skirmisher/TA-Patch-Installers | NSIS | 2015-03-14 (dead) | NSIS installer sources for the TA Unofficial Patch; binary payloads deliberately omitted. | none | **[VERIFIED]** |
| gammata/mayhem | gammata | https://github.com/gammata/mayhem | GLSL | 2023-07-28 | The Mayhem mod (TADR has a `config_mayhem.h`). References `TotalA.exe` in `TotalMlatest/readme.MD`. | GPL-3.0 | **[VERIFIED]** |

## Tier 4 — Reverse-engineering artefacts

| Name | Author | Link | Lang | Last activity | Notes | Licence | Conf. |
|---|---|---|---|---|---|---|---|
| **totala-re** | ioma8 | https://github.com/ioma8/totala-re | Python | 2025-10-17 (12 commits) | Only public standalone RE project for `TotalA.exe`. **radare2**-based. Docs: `DISASSEMBLY_NOTES.md`, `FUNCTION_CATALOG.md`, `DATA_STRUCTURES.md`, `MAIN_LOOP_ANALYSIS.md`, `ENGINE_OVERVIEW.md`. Shallow: 391 functions enumerated, entry `0x0049eda0` / `entry0 0x004e6fa0`. HPI/SQSH parser, TMH→WAV, pygame `.GUI` renderer. Stated goal is a **Rust reimplementation**, not patching. | none stated | **[VERIFIED]** |
| **TADR RE notes (in-repo)** | Axle1975 et al. | `ta info.txt`, `ta entry point.txt`, `src/IDAnalyze/`, `src/Docs/` in tanvanman/TADR | Delphi + text | 2026-08-31 | Far deeper than totala-re: IDA-style annotated addresses (`.text:00429D86 mov [ecx+TAdynmemStruct.Cursor_Teleport], eax`), named structs (`UnitDefStruct.UnitBitfields` at `0x241`, bit `0x2000` = Is Teleporter), dispatcher addresses (`loc_43F813` teleport, `sub_40FBE0` unit orders, `480770 UnitScripting_Handler_Get`). `src/Docs/` has `PACKETS.TXT`, `TANET.TXT`, `lobbyprot.txt`, `saveformat.txt`, `newfeatures.txt`. `src/CallstackParser`, `src/IDAnalyze` (Delphi) are analysis helpers. **This is the de-facto TA symbol database.** | MIT (repo LICENSE) | **[VERIFIED]** |
| Cheat Engine table — TA v3.1.0.0 Steam | Recifense | https://fearlessrevolution.com/viewtopic.php?t=105 | CE table | published 2015-12-31 | Full Resources, God Mode, "Some Pointers" against process `TotalA.exe`; implies mapped resource/unit structures. | n/a | **[LEAD]** (seen in search results only, page not opened) |

## Active TADR forks (signal that the project is alive)

| Fork | Last push |
|---|---|
| https://github.com/sc0tt88/TADR | 2026-08-30 (open PRs merged upstream: megamap projectiles/range displays, megamap hit coordinates) |
| https://github.com/dv-morais/TADR | 2026-08-30 |
| https://github.com/zlatkok/TADR | 2026-07-31 |
| https://github.com/tagROCK/TADR | 2026-06-23 |
| https://github.com/MnHebi/TADR | 2025-06-13 |

TADR contributor commit counts: Axle1975 506, FunkyFr3sh 36, tanvanman 26, sc0tt88 17,
tagROCK 16, dv-morais 14, KevinHake 2, zlatkok 1. CI: `.github/workflows/compile.yml`.

---

## NEGATIVE RESULTS (searched, nothing found)

- **No public Ghidra project, .gzf, or Ghidra script repo for `TotalA.exe`.** GitHub code
  search and web search returned only generic Ghidra documentation. The only public
  disassembly effort is `ioma8/totala-re` (radare2).
- **No public IDA `.idb`, IDA plugin, or IDA-Python repo for TotalA.exe.** TADR contains
  IDA-*derived* notes and a Delphi `IDAnalyze` tool, but no database is published.
- **GitLab: nothing.** GitLab API searches for `total annihilation`, `totala`, and
  `annihilation` returned only Spring-engine Balanced Annihilation mirrors and unrelated
  D&D/Minecraft projects. No TA binary-patching project on GitLab.
- **SourceForge: nothing on-topic.** Directory search surfaced only re-implementations
  (TA3D, OpenGL TA Project, meCpp) and Spring mods (Absolute Annihilation, TA: Twilight).
  DxWnd is the one relevant SourceForge project and it is a generic hooker.
- **`gh search repos "tademo"`** → 20 results, all unrelated (corporate "TA demo" repos).
- **`gh search code '"Total Annihilation" WriteProcessMemory'`** → zero hits. No external
  process-injection trainer/loader for TA on GitHub.
- **No DirectPlay *replacement* implementation specific to TA** beyond the proxy DLLs above;
  `gh search code "tplayx"` returns only `tanvanman/TADR` plus unrelated `dplay.h` SDK copies.
- Original standalone TA Demo (Fnordia, `ta.lfjr.net/recorder/`) has no separate repo — its
  lineage is absorbed into TADR (`src/Server` is MIT © 2015 Chaos, © 2003 Fnordia).

## OUT OF SCOPE (noted so they aren't re-researched)

Re-implementations / asset tools, no exe patching: `MHeasell/rwe` (2026-08-28),
`mackron/openta`, `zuzuf/TA3D` (2026-08-07), `loganjones/nTA-Total-Annihilation-Clone`,
`loganjones/SwiftTA`, `coreprime/*` (kbot toolchain — Go/WASM sim engine, format parsers,
2026-07), `sidav/totala_reader` (Go, 2024-09), `THGSCST/HPIZ-Archiver` (C#, 2026-08-18),
`btigi/TaView` · `btigi/hpiex` · `btigi/hpiconex` · `btigi/iiCompleteDestruction` (C#),
`MHeasell/Mappy` (map editor, 2025-02), `brycesandlund/Total-Annihilation` (Java, 2015),
`Axle1975/3do2scm`, `Axle1975/SCTA-v18`.

## Follow-ups worth doing

1. `svn.riouxsvn.com/tadr` — the pre-GitHub TADR SVN named in the README. Not fetched. [LEAD]
2. Read `src/DDraw/config_*.h` in TADR to map exactly which engine features are gated per mod
   (OTA / ProTA / Escalation / Mayhem / BTA / TA Zero).
3. `src/DDraw/ESCALATION_SHARE_GUARD_DESIGN.md` — an in-repo design doc, likely the best
   worked example of how a new feature gets added.
4. Confirm cnc-ddraw's SPDX licence.
5. tauniverse.com forums (TA Unofficial Patch subforum, `f=170`) — not swept here; that is the
   forum-side angle.
