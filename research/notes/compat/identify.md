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

- **The "TADR engine-limit error" box** is a 2026 TADR refusing beside Impure ([the refusal](tadr-collision.md#2026-tadr-enginelimits-refuses)).
- **"Total Annihilation: Impure cannot start"** and `log\startup-failure.txt` are Impure
  refusing: the report lists every site that differed. `0x0040EAD6 pathfinding budget` alone
  means an exe with a changed path budget — the 3.9.02 exe (66650) or Escalation's (1114).
- **`ErrorLog.txt`** is the engine's own crash report. An access violation at `0x004C9396`
  writing `0x0042C029` in the Load Thread is [the pre-2026 limit crack beside Impure](tadr-collision.md#the-crash-read-from-the-running-process).

## A worked example: the TA Zero report

The report of 2026-09-26 decoded this way: the retail 3.1 exe, a Patch Loader newer
than v1.3.0.0, TADR's `tazero` tdraw from `dev-ddc51e4` or `dev-dcff5dd`, the 2021–2025
recorder, a bass-based `win32.dll`, and Impure v0.2.3. The setup `loader+tadr-tazero`
reproduces it: the same tdrawlog line on Wine, the same box on the Windows test box's screen.

