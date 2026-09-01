# Candidates — engine features added to ORIGINAL Total Annihilation (1997 `TotalA.exe`)

Discovery pass, feature-first. Scope: projects that patch/hook the closed-source Cavedog binary.
**Out of scope and excluded:** Spring/Recoil games (BAR, Zero-K, Balanced Annihilation), TA3D, OpenRA,
and pure content mods with no engine work.

Confidence: **[VERIFIED]** = primary source read directly (source code, project site, changelog).
**[LEAD]** = plausible, mechanism or scope not confirmed.

---

## Headline finding

The real engine-modding scene for OTA is a **shared corpus of hex patches against `TotalA.exe` v3.1**,
maintained in the open in `FunkyFr3sh/Total-Annihilation-Patch-Loader`. Three projects ship distinct
patch sets from that corpus: the **Community/Unofficial Patch**, **ProTA**, and **Total Mayhem**.
The loader ships as a `dplayx.dll` shim that applies a byte-patch table from an INI at load time, so
mods no longer redistribute a modified exe. On top of that sits **Axle1975's `ddraw.dll` patch**, which
is where the genuinely new gameplay/UI capability lives today.

---

## Candidate table

| # | Name | Author / team | Dates / last activity | Engine feature(s) added | Mechanism | Link | Conf. |
|---|------|---------------|----------------------|-------------------------|-----------|------|-------|
| 1 | **TA Unofficial / Community Patch 3.9.x** | TAUniverse community (installers by Skirmisher; modern drop-in by gammata) | 3.9.01 beta ~2013; 3.9.02 beta; loader string `v2023` | Unit-limit raises (500 / 1500 / 5000); Expanded Battleroom + Map Selection GUI; pathfinding fix; LOS tables fix; `+atm 10000` patcher; Multicore Patch; sound `mixingbuffers` increase; desktop-resolution + windowed mode; demo recorder + Replayer; megamap mousewheel zoom-out strategic view (full command from zoomed-out) | **Hex-edited `TotalA.exe` → 3.9.x**, plus `ddraw.dll` and `dplayx.dll` | [thread t=43735](https://www.tauniverse.com/forum/showthread.php?t=43735) · [notes.txt](https://github.com/Skirmisher/TA-Patch-Installers/blob/master/Patch/notes.txt) · [drop-in](https://github.com/gammata/TA-Unofficial-Patch-Install) | VERIFIED |
| 2 | **Total Annihilation Patch Loader** | FunkyFr3sh | commits Oct 2023 – May 2024, MIT | Infrastructure: applies named byte patches at runtime. Built-in toggles: `CommanderOrdersNotReset`, `ChangePathfindingSearch`, `DirectxPopupElimination`, `CursorReclaim`, `DisableKeyLastCommandRepeat`, `EnableF10Debug`, `IncreaseAtmToFillResources`, custom registry path / config file / GP3 name / download path / version string / MP version | **DLL shim (`dplayx.dll`) + INI patch table** — explicitly "made to replace the hex edited TotalA.exe" | [repo](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader) | VERIFIED |
| 3 | **ProTA 4.4 / 4.5** | Venom (TAG_Venom); patches organised into loader Oct 2023 | 4.5 ~Apr 2023 | `init_cloaked` units not cloaked until built; weapons acquire targets while attacking-in-sight-range, while D-gunning ground, and (Mayhem-shared) while building/nanolating/reclaiming/repairing; **eliminate target-locking**; AI commander orders not reset when attacked; AI unit-count threshold before commander waits; AI stockpile purchasing fix; AI turns off energy-hungry buildings when low; reclaim cursor over any unit; anonymous commanders on minimap; resurrection units can reclaim on command; Allied Victory on by default; F10 debug mode; `\` and Insert key remap (frees `\` for demo whiteboard); AI group-5 → group-1 behaviour (contributed by **Rahsennor**); `+atm` fills storage; build hotkeys + build-menu hotkey overlays | Hex patches via loader (`prota.ini`) | [prota.ini](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader/blob/master/res/prota.ini) · [t=46810](https://www.tauniverse.com/forum/showthread.php?t=46810) | VERIFIED |
| 4 | **Total Mayhem** (10.9.2 / 11.3.0) | Mayhem Inc. — "gamma", + Rahsennor, Axle1975, M1Garland | site 2001–2024; 11.3.0 released 29 Jun 2024 | Largest patch set found (951-line INI). Superset of ProTA plus: **teleport order button** — exposes TA's dormant teleport logic as a clickable command (code by **Rahsennor**, `teleporter=1` FBI tag); AI resource-income & feature-reclaim multipliers by difficulty; `setSFXoccupy` hacks incl. submerged structures; `healtime` self-heal delay; repair-pad "OFF" fix; guards don't chase while on Hold Position; pathfinding `+search` initial value raised; AI can use nukes/antinukes; per-mod `guisM`/`unitsM`/`weaponM`/`unitpicM` folder redirection; CTRL-Z crash fix; full scoreboard for departed players; **extended builder interface: 10 buttons per page instead of 6** (still works at 800x600) | Hex patches via loader (`mayhem.ini`); ships an unmodified `totala.exe` | [mayhem.tauniverse.com](https://mayhem.tauniverse.com/) · [mayhem.ini](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader/blob/master/res/mayhem.ini) | VERIFIED |
| 5 | **"New ddraw patch"** (for Mayhem / TAF) | **Axle1975** | shipped with Mayhem 11.3, Jun 2024 | Verbatim from release notes: synced windspeed between internet players; ghost-commander bug fix; **unit-ID recycling bug fix**; `+autoteam`/`+randomteam` + auto player positioning; `.crcreport` version checking; `.autopause` in battleroom; `+shootall`/`+noshake`/`.ready` in-game buttons; cursor click-snap to metal spot / geo vent / reclaim; weather report + game clock on top bar; **move queued orders around the map**; allied-victory fix; render pre-placed dragon's teeth; movable map markers; F11 chat-macro relay disabled (local-only); minimum default resolution 1024x768; local-only `+logo` colours; whiteboard markers follow `+logo`; ctrl-b = construction units only; ctrl-f = idle factories only + zoom; `+lostype` moved to cheatcodes; **improved megamap FPS**; **radar/sonar jammer fix in games with >3 players** | **`ddraw.dll` replacement/hook** | [release notes, 29 Jun 2024](https://mayhem.tauniverse.com/) | VERIFIED |
| 6 | **TA Forever (TAF)** — `gpgnet4ta` | ta-forever org; **Axle1975** primary (207 commits) | active, client v2026.8.23; 53,685 battles on record since 2023 | Modern online play by hooking TA's DirectPlay: lobby/launcher integration, replay recording + serving (`tareplay`, `tapacket`, `jdplay`, `dplayreg` libs), spectating, ladders, on-demand mod install (OTA, ProTA, Escalation, Zero, Twilight) | Launcher + DirectPlay hook/DLL; **not** a re-implementation | [github](https://github.com/ta-forever/gpgnet4ta) · [taforever.com](https://www.taforever.com/) | VERIFIED |
| 7 | **TA: Escalation (TA:ESC)** | TA:ESC team (built on Talon + TAWP; credits Zwzsg, Sinclaire, Archdragon, Storm, Zodius) | site updated Aug 2022; beta 9.9.8 Aug 2024 | **Deflective shield generators** blocking all but the heaviest weapons; **mass unit teleportation between galactic gates**; upgradable buildings; energy adjacency bonuses; mass load/unload transports; multi-unit air transports; surfacable subs with nuclear warheads; complete L3 tier; extended menu/GUI requiring ≥1024x768 | **Unclear.** Ships its own `TA Escalation/totala.exe`. FAQ attributes these to "advances in computing power", i.e. heavy COB scripting — *not* claimed as an exe patch | [taesc.tauniverse.com](https://taesc.tauniverse.com/index.php?p=intro) | Features VERIFIED; mechanism **LEAD** |
| 8 | **xpoy — TA Interface Upgrade** | xpoy | pre-2013; superseded by 3.9.x | Resolution / interface upgrade for OTA | `ddraw.dll` | cited in [notes.txt](https://github.com/Skirmisher/TA-Patch-Installers/blob/master/Patch/notes.txt) | VERIFIED (ref) |
| 9 | **TA Demo / TA Demo Recorder** | The Swedish Yankspankers (0.99b2); update 1.0.0.545 by **Xon** | pre-2013 | Replay/demo recording for OTA | `dplayx.dll` shim | cited in notes.txt | VERIFIED (ref) |
| 10 | **Modular-Patch** | Thaldren-Updates (GitHub) | Dec 2025 | Modular DLL replacements: DDRAW (DirectX init failures, full-screen window) done; DPLAYX (network) and WINMM (music) WIP | `ddraw.dll` drop-in | [repo](https://github.com/Thaldren-Updates/Modular-Patch) | VERIFIED |
| 11 | **totala-re** | ioma8 | Oct 2025 | "Reverse engineering toolkit for Total Annihilation engine & resources" — enabling infrastructure, not a feature mod | RE tooling | [repo](https://github.com/ioma8/totala-re) | LEAD |
| 12 | **TA 4GB Patch** | unknown | Sep 2020 | Large-Address-Aware flag so `TotalA.exe` can address 4GB | PE header patch | gamepressure mirror | LEAD |
| 13 | **TA Mutation** | unknown | unknown | Referenced in patch-installer cleanup notes as a separate exe-modifying package that is hard to uninstall | modified exe (implied) | notes.txt | LEAD |
| 14 | **TA-Redux-Mod-Dropin** | Thaldren-Updates | Jul 2026 | Unknown — a TA mod, engine content unconfirmed | unknown | [repo](https://github.com/Thaldren-Updates/TA-Redux-Mod-Dropin) | LEAD |
| 15 | **TA Zero / Twilight / TA:Enhanced / The Secret Forces** | various | Zero alpha 4d & TA:Enhanced 0.8v ~2020; Twilight active on TAF | On TAF's integrated-mod roster; unknown whether any ship engine patches | unknown | taforever.com mod list | LEAD |
| 16 | **`TotalA_exe-Rewrite`** | cjg38340 | Jun 2026 | "Engine rewrite to reproduce exact behavior of the original" — borderline: a re-implementation, but derived from the original binary | rewrite | [repo](https://github.com/cjg38340/TotalA_exe-Rewrite) | LEAD (likely out of scope) |

---

## Named individuals doing binary work

FunkyFr3sh (loader, cnc-ddraw) · **Axle1975** (ddraw patch, TAF launcher — the most active engine dev today) ·
**Rahsennor** (teleport order button, AI group behaviour — writes raw patches on request) ·
**Venom / TAG_Venom** (ProTA patch set) · **gamma** (Total Mayhem) · xpoy · Xon · Skirmisher · gammata ·
SnakeInTheMirror/Keeper/MnHebi (basswasapi music player).
The community hub thread is TAUniverse ["Hacking TA..."](https://www.tauniverse.com/forum/showthread.php?t=41608)
(cited by offset in both `prota.ini` and `mayhem.ini`). **Note: tauniverse.com and moddb.com are behind
Cloudflare and could not be fetched; web.archive.org is unreachable from this environment.** Those two
sites are the biggest remaining unread primary sources.

---

## Negative results — searched for, no evidence found

- **Shields as an *engine* feature — NOT FOUND.** This is the headline negative. TA:ESC's shields are real
  and shipped, but nothing in ~1,400 lines of documented binary patches across ProTA + Mayhem + the
  community patch mentions shields, damage absorption, or bubbles. No hit anywhere for a shield opcode
  patch. Working hypothesis: TA:ESC shields are COB-script + unit-data engineering (weapon/armour class
  trickery), not an engine addition. **Needs confirmation from the TA:ESC changelog or its devs.**
- **New weapon *types* / classes — NOT FOUND.** Every weapon-related patch found is targeting/acquisition
  *behaviour* (when a unit may acquire a target), never a new weapon class.
- **Player count above the stock 10 — NOT FOUND.** No one appears to have raised it. 10 is stock;
  Mayhem's jammer fix references ">3 players" only as a bug boundary.
- **New unit categories / new movement classes — NOT FOUND.**
- **Terrain or larger maps — NOT FOUND.** Pathfinding work is limited to raising the `+search` depth
  value and a LOS-table fix. No map-size patch.
- **True multi-threading — NOT FOUND.** The community patch's "Multicore Patch" is almost certainly CPU
  affinity pinning (a compatibility fix), not parallelism. Performance gains are claimed via bugfixes
  ("more units ingame with less lag") and "improve megamap FPS", not threading.
- **Higher game speeds — NOT FOUND.**
- **Cloaking / radar / jamming beyond stock — NOT FOUND.** Only bug fixes (`init_cloaked` timing,
  jammers with >3 players), not new capability.
- **Extra build pages — NOT FOUND.** Mayhem's 6→10 buttons per page is the only build-menu capacity
  change; no evidence of additional pages.
- **New stockpiling mechanics — NOT FOUND.** Only AI stockpile-purchase fixes.
- **AI in the binary — FOUND, and substantial.** Contrary to expectation, a large share of ProTA's and
  Mayhem's patches are AI changes made *in the executable*, not in AI profile files: commander order
  retention, difficulty-scaled resource multipliers, nuke/antinuke usage, stockpile purchasing,
  energy-appliance management, squad targeting rules.

## Suggested next steps

1. Get past Cloudflare on `tauniverse.com` thread **t=41608 ("Hacking TA...")** — the primary technical
   record of who reversed what, and where a shields attempt would surface if one exists.
2. Read the **TA:ESC changelog** (bundled in the download readme) to settle the shields mechanism.
3. Find **Axle1975's ddraw patch source** — not in `ta-forever/gpgnet4ta` or FunkyFr3sh's repos; may be
   closed or hosted on tauniverse. It is the most capable OTA engine extension found.
