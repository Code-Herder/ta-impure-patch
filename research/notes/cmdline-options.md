# Command line, totala.ini and movie files — every external launch knob in the stock exe

*Complete enumeration of the launch-time configuration surface of our pristine build
(ImageBase `0x400000`, md5 per `pristine/manifest.md5`, Steam 3.1): command-line
switches, `totala.ini` options, and the intro-movie file mechanism. Compiled 2026-09-01
for the tacli instance-launcher work. All addresses are VAs for this build; disassembly
via `objdump -d -M intel` on `pristine/TotalA.exe.pristine`.*

Evidence tags as in `terrain-depth.md`: **[BINARY-VERIFIED]** = instructions read for
this build. **[COMMUNITY]** = documented by the community (official readme mirror /
PCGamingWiki), not yet runtime-confirmed on our stack. **[INFERRED]** = my reading.

## TL;DR for the launcher

- **Sound off**: `totala.ini` next to the exe, `[Preferences] NoDirectSound=1`
  (official, per-gamedir — no registry, no wine audio hacks). Switch form `-s`.
- **Intro movies off**: don't symlink `Data/1.ZRB` and `Data/2.zrb` into the
  instance mirror — the play call is find-file-gated and skips gracefully.
- **Resolution**: registry `DisplaymodeWidth/Height` only (see `resolution.md`) —
  the stock exe has **no** resolution switches (`-screenwidth` is a 3.9.02-patch
  feature, absent from this binary).
- **Windowed**: TA's own `-d` switch exists but likely bypasses/perturbs the ddraw
  path our whole stack lives in — use cnc-ddraw `windowed=true fullscreen=false`
  instead and treat `-d` as off-limits until tested.

## totala.ini — the complete option list [BINARY-VERIFIED]

Read via `GetOption(default, name)` = `0x49F5A0`: builds `"<exedir>\totala.ini"`
(format string `0x5098A0`) from `GetModuleFileNameA`, then
`GetPrivateProfileIntA("Preferences", name, default, path)` (section string
`0x509894`). The exe directory is the *instance gamedir* in our layout, so the INI
is naturally per-instance.

Every call site of `0x49F5A0` in the binary — i.e. the **entire** supported set:

| Option | Call site | Effect |
|---|---|---|
| `NoDirectSound` | `0x47ED4A` | `!=0` → global `0x51E690=1`, DirectSound never initialised (silence). Official (readme). |
| `UseWindowsSound` | `0x47ED64` | `!=0` → global `0x51E694=1`, waveOut instead of DirectSound (no briefing narration/movie audio). Official (readme). |
| `UnitLimit` | `0x491653` | Per-player unit cap (community-famous 250→500 raise). |

Nothing else reads the INI. There are no display/movie/window INI options.

## Command-line switches [BINARY-VERIFIED]

Parser at `0x49EEC0`-ish, token loop: a token starting with `-` or `/`
(`0x49EEDD`) dispatches on the *next* character, case-insensitive, via jump table
`0x49F494` + index bytes `0x49F500` (covering `'B'..'w'`). Letters not listed
below fall through to the default (ignored). Numeric/string arguments may be
glued (`-tNN`) or the next token (`-t NN`).

| Switch | Handler | Semantics |
|---|---|---|
| `-b <word>` | `0x49EF3A` | Battle/game-rule words, each `strcmp`'d in sequence: `lock` (sets bit in `main+0x2C74`), `deathends`, `deathplays`, `deathmatch`, `fixedloc`, `mapping`, `circlos`, `truelos`, `permlos`. Multiplayer/battleroom rule presets from the command line. **[INFERRED]** semantics per word not traced; promising registry-free alternative for LOS/mapping presets — trace before use. |
| `-c <str>` | `0x49F05D` | String arg → `0x45B670(str)` — loads a file (community lore: launcher/spawn config). **[INFERRED]** |
| `-d` / `-df` | `0x49F0B2` | Sets display-mode global `0x51FB48` = **3** (`-d`) or **2** (`-df`). Community: `-d` = **windowed mode**, side effect mutes sound. Corpus label for `0x51FB48` (`gLanguageScratch`) looks wrong. **[COMMUNITY]** — see stack warning above. |
| `-e <n?>` | `0x49F0DE` | Numeric arg (accepts `-`-prefixed negatives). Target not traced. **[INFERRED]** |
| `-f` | `0x49F136` | `0x41D4B0(ebx)` — untraced. |
| `-h <str>` | `0x49F141` | String arg → `0x45B820(ebx, str)` — untraced (host?). |
| `-l` | `0x49F18B` | `0x41D4A0(0)` — untraced (same family as `-f`). |
| `-n <n?>` | `0x49F197` | Numeric arg — untraced. |
| `-p <n?>` | `0x49F202` | Numeric arg — untraced. |
| `-r` | `0x49F249` | Re-parses following chars — untraced. |
| `-s` | `0x49F402` | `0x47EFC0()` → NoDirectSound (same global as INI). **Official: disable all sound.** |
| `-t <n>` | `0x49F409` | Numeric, clamped to `30..300` (else 30), stored `main+0x37F31`. **[INFERRED]** untraced meaning. |
| `-w` | `0x49F45C` | `0x47EFD0()` → UseWindowsSound (same global as INI). Official. |

The debug-runtime switches visible in strings (`-dprinton`, `-dprintfile`,
`-memfussy`, `-gonzo`, …) belong to the CRT/debug layer, parsed elsewhere; they
are not part of this dispatch and not useful to the launcher.

## Silencing the game completely [RUNTIME-VERIFIED 2026-09-01]

`NoDirectSound=1` is **not enough**. It kills the DirectSound effects path only; the
Steam build still plays its MP3 soundtrack through `audiere.dll` from `gamedir/music/`,
which shows up as a live PulseAudio client ("Cavedog Entertainment's Total Annihilation",
binary `wine`) once you are in a game. Menus are silent, so the gap is easy to miss.

Full silence = INI + registry + assets:

1. `totala.ini`: `[Preferences] NoDirectSound=1`
2. Registry, under `HKCU\Software\Cavedog Entertainment\Total Annihilation` (all
   `REG_DWORD`, all observed live in our prefix):

   | Value | Set to | Controls |
   |---|---|---|
   | `musicmode` | 0 | music playback on/off |
   | `musicvol` | 0 | music volume (default 0x20) |
   | `fxvol` | 0 | sound-effect volume (default 0x1B) |
   | `cdmode` | 0 | CD-audio mode (default 4) |
   | `ackfx` | 0 | unit acknowledgement voices |
   | `buildfx` | 0 | construction sounds |

3. Omit the `music/` directory from the instance mirror — audiere cannot play files that
   are not there (belt and braces, and it is free in a symlink mirror).

Verified: with all three in place, `pactl list sink-inputs` shows **zero** TA clients at
the menu *and* in a running skirmish.

Also in that key: **`PlayMovie` (REG_DWORD)** — a second intro lever alongside the ZRB
file gate; tacli sets it to 0.

## Intro/outro movies — Data/*.zrb [BINARY-VERIFIED file gate, COMMUNITY skip]

Movies are Smacker streams shipped as `Data/1.ZRB … 5.zrb` (all five names are
in the exe). The intro sequence call site `0x425ECF`:
`push "INTRO"; call 0x49FD60; test eax; je 0x426065` — i.e. *find the movie
asset; if the lookup fails, skip past playback entirely* (the graceful path the
CD-less crowd has relied on for decades: delete/rename `1.ZRB` + `2.zrb`).

For our per-instance symlink mirrors this is free: **omit the two symlinks** and
the intro never runs. Keep `3/4/5.zrb` (endgame movies) linked. No patch needed;
if a patch is ever preferred, forcing that `je` unconditional is a one-byte
`tagpu_patches.c` entry in the proven pattern (`binary-patches.md`).

Related strings: TA refuses movie playback when it believes it is windowed
("You must be in full-screen mode to play a movie") — irrelevant under cnc-ddraw
(the game always believes it is fullscreen), but another reason `-d` is suspect.

## What does NOT exist in this binary

- No resolution switches (`-screenwidth`/`-screenhight` = unofficial 3.9.02 only;
  strings absent here). Resolution stays registry-driven (`resolution.md`).
- No intro/movie/nointro switch or INI option (`nomovie` in strings is a mission
  **TDF key** read by the TDF map-lookup at `0x4C46C0`, near `missiondescription`).
- No windowed INI option; only the `-d` switch above.

## Sources

- Official readme (Steam `gamedir/readme.txt`; mirror taguide.tauniverse.com/pages/readme.html) — documents `-w`/`-s` + `totala.ini` mechanism only.
- PCGamingWiki "Total Annihilation" — ZRB deletion for intro skip, `-d` windowed lore, 3.9.02 `-screenwidth`.
- Binary enumeration this session (switch table decode + `GetOption` call-site sweep) — the authoritative list above.
