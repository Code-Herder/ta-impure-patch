# Identifying a player's setup

## Summary

A player sends `tdrawlog.txt`, an error box or `log\startup-failure.txt`, rarely a version
number. The config name in TADR's log, the image size of each DLL in its module list, and the
title of the box are enough to name the builds, and so the [setup](setups.md) that reproduces
the report.

## What each piece of evidence gives

- **`tdrawlog.txt`** is TADR's. The `config=` value names the build's mod (`tazero`, `mayhem`,
  `prota`, `ota`, `escalation`); `[EngineLimits]` lines mean a 2026 build; "Install Limit Crack"
  means an older one. Its module list gives each DLL's base and **image size**, which
  identifies the build when there is no version string:

  | file | image size | build |
  |---|---|---|
  | `tdraw.dll` | `0x296000` | TADR 2026 dev builds (`dev-dcff5dd`, `dev-ddc51e4`) |
  | `tdraw.dll` | `0xB6000` | ProTA 4.8's 2025 build (limit crack) |
  | `tdraw.dll` | `0x9F000` | Total Mayhem 11.3.0's 2024 build (limit crack) |
  | `dplayx.dll` | `0x2C000` | Patch Loader v1.3.0.0, the public release |
  | `dplayx.dll` | `0x31000` | the newer loader ProTA 4.8 ships; also in the TA Zero report |
  | `dplayx.dll` | `0x18000` | Total Mayhem 11.3.0's own loader build |
  | `tplayx.dll` | `0x4F000` | the recorder builds of 2021–2025 (gammata's 2022 pack, Mayhem 11.3.0, ProTA 4.8) |
  | `tplayx.dll` | `0x57000` | the 2026 recorder (TADR's `src/Recorder/dist`) |

- **Whether a DLL is TADR at all**, whatever it is called: its file carries the string
  `TADemo-MKChat`, the name TADR's builds give their chat channel. MEASURED 2026-09-27 over the
  suite's 130 fixture files: in all 18 TADR modules — every `tdraw.dll` and its renamed copies
  (Escalation's `TAESC.dll`), every recorder (`tplayx`, `eplayx`, `zplayx`, the 2006 `Dplayx.dll`)
  — and in nothing else, including the Patch Loader's `dplayx.dll`, Total Mayhem's and ProTA's own
  `dplayx.dll`, cnc-ddraw's `ddraw_custom.dll`, the audio DLLs and every mod's `TotalA.exe`.
  `TA Demo Recorder`, in the 9 recorders only, separates a recorder from a tdraw. This is the test
  Impure itself uses ([the takeover](takeover.md), part 1), never the file's name.

- **The "TADR engine-limit error" box** is a 2026 TADR refusing beside Impure ([the refusal](tadr-collision.md#2026-tadr-enginelimits-refuses)).
- **"Total Annihilation: Impure cannot start"** and `log\startup-failure.txt` are Impure
  refusing: the report lists every site that differed. `0x0040EAD6 pathfinding budget` alone
  means an exe with a changed path budget — the 3.9.02 exe (66650) or Escalation's (1114).
- **`log\tagpu.log`**'s `takeover:` lines are the whole story of what TADR was allowed to do.
  `asked for tdraw.dll … answered with Impure` means the Patch Loader route was taken over, so
  there is no `tdrawlog.txt` from this launch; `its entry point was made inert` names a TADR module
  that was loaded and never ran; `the exe's code against its file` closes with how many places of
  the game's code lead into a TADR module (0 is the goal) and how many into another DLL of the
  folder (the mod's own, and each is listed with its bytes). Without those lines — an older build,
  or `tagpu_takeover.off` in the folder — a launch with no `tdrawlog.txt` and no recorder log is
  *not* a launch with no TADR code: the recorder's entry-point way in leaves nothing in the folder.
- **"Total Annihilation's code has been changed in memory by TA Demo Recorder"** is the
  reference-image check: TADR got in by a route the takeover does not cover, and the report lists
  each site with its bytes and the file's.
- **"Impure's engine limits and fixes were changed by another program"** is the safety net:
  a patcher that started after Impure rewrote the sites listed, and the game stopped before a
  battle could crash on them.
- **`ErrorLog.txt`** is the engine's own crash report. An access violation at `0x004C9396`
  writing `0x0042C029` in the Load Thread is [the pre-2026 limit crack beside Impure](tadr-collision.md#the-crash-read-from-the-running-process)
  — Impure v0.2.3; a later build takes that route over or stops at start-up.

## A worked example: the TA Zero report

The report of 2026-09-26 decoded this way: the retail 3.1 exe, a Patch Loader newer
than v1.3.0.0, TADR's `tazero` tdraw from `dev-ddc51e4` or `dev-dcff5dd`, the 2021–2025
recorder, a bass-based `win32.dll`, and Impure v0.2.3. The setup `loader+tadr-tazero`
reproduces it: the same tdrawlog line on Wine, the same box on the Windows test box's screen.

