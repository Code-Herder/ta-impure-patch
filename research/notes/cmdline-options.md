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
- **Game rules**: **nothing on the command line sets them.** `-b` is broken in the
  stock binary and its words are empty anyway (see "The `-b` dead end"); the rule
  path that does exist is `online.dll`, and it is network-only. Skirmish rules stay
  on the registry.
- **Switches worth passing at all**: `-s`/`-w` (sound), `-t`/`-e`/`-p` (network
  debug knobs), `-n` (jump to a DirectPlay dialog). Everything else is a no-op, a
  trap (`-r` registers DirectPlay and quits; `-f` fakes a CD drive letter), or
  off-limits (`-d`). `tacli launch --arg=…` refuses `-r` and `-d`.

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

## Command-line switches [BINARY-VERIFIED + RUNTIME-VERIFIED 2026-09-01]

Parser at `0x49EEC0`, token loop over `strtok(cmdline, " \t")` (`0x4E5280`). A token
starting with `-` or `/` (`0x49EEDD`) dispatches on the *next* character,
case-insensitive, via jump table `0x49F494` + index bytes `0x49F500` (covering
`'B'..'w'`; every unlisted letter maps to the loop tail, i.e. is silently ignored).
Any other token is copied to `0x51FB50`. Each handler advances `edi` past the two
switch characters itself, so numeric/string arguments may be glued (`-t120`) or the
next token (`-t 120`) — **except `-b`, which forgets to, see below**.

Three globals are initialised before the loop: `main+0x37F31 = 30`,
`main+0x37F35 = 0`, `main+0x39245 = 0` (`main` = `*0x511DE8`, `TAdynmemStructPtr`).

| Switch | Handler | Writes | Semantics |
|---|---|---|---|
| `-b <word>` | `0x49EF3A` | — | **Broken, does nothing.** See "The `-b` dead end". |
| `-c <str>` | `0x49F05D` | `0x512C80`-block, `main+0x39245=1` | `online.dll!ONLLoadConfigFile(str, block, 0x150)` — the lobby-launch hook. See "`online.dll`". |
| `-d` / `-df` | `0x49F0B2` | `0x51FB48` = 3 / 2 | Engine display mode. **Off-limits** for our stack (see warning above). |
| `-e <n>` | `0x49F0DE` | `main+0x37F35`, clamp 0..100 | **Simulated packet loss, percent.** Read at `0x44FCBF` in the packet-send path: `if (pct) { if (rand()%101 <= pct) drop }`. Debug switch. |
| `-f` | `0x49F136` | `0x511DE0 = 1` | **Fake the CD.** Read at `0x41D6EA`: skips the A..Z `GetDriveTypeA`==`DRIVE_CDROM` search (`0x4BB190`) and reports drive `'h'` instead. Do not use — the Steam build needs no CD and a bogus letter can only hurt. |
| `-h <str>` | `0x49F141` | `0x512C84=1`, `0x512CA8` (63 B) | Name string in the lobby block; used as the 16-char player/session name at `0x4577A6`. Rejects an argument starting with `-`. |
| `-l` | `0x49F18B` | `0x502898 = 0` | **Dead switch**: writes a flag (init 1) that nothing in the image reads. |
| `-n <n>[:<str>]` | `0x49F197` | `0x512C80 = n`, `main+0x39245=1` | **DirectPlay connection type**, `n` in 1..4 (others ignored) — jumps the frontend straight to SELECT CONNECTION with that provider's dialog open: **1 = Internet TCP/IP, 2 = IPX, 3 = Modem, 4 = Serial**. With `n == 1` a `:`-suffix is copied to `0x512D90`. |
| `-p <n>` | `0x49F202` | `0x513004` | **Network send pacing.** `n<0` → `0x506DBC=0`; `n==0` → 200 ms; else clamp 2..30 and store `1000/n` ms ("setting m_defaultSendPacingMs to: %lums"). Default 200. |
| `-r` | `0x49F249` | — | **DirectPlay registration, then quits.** Loads `dsetup.dll`, calls `DirectXRegisterApplicationA`, message-boxes "DirectPlay registration failed." on error, and returns 0 from the parser. An install-time switch. **Never pass it.** |
| `-s` | `0x49F402` | `0x51E690` | `0x47EFC0()` → NoDirectSound (same global as the INI). **Official: disable all sound.** |
| `-t <n>` | `0x49F409` | `main+0x37F31`, clamp 30..300 | **Battleroom join timeout, seconds** (default 30) — feeds "will be rejected in %d seconds" at `0x453859`. |
| `-w` | `0x49F45C` | `0x51E694` | `0x47EFD0()` → UseWindowsSound (same global as the INI). Official. |

The debug-runtime switches visible in strings (`-dprinton`, `-dprintfile`,
`-memfussy`, `-gonzo`, …) belong to the CRT/debug layer, parsed elsewhere; they
are not part of this dispatch and not useful to the launcher.

### Measured A/B [RUNTIME-VERIFIED 2026-09-01]

One instance, launched twice, globals read in-process with `tacli peek` (the
`tagpu_peek.trigger` instrument). All the switches under test rode the **same**
command line, so the working ones are the positive controls that make the `-b`
null result mean something.

| Global | Switch | Baseline | With switches |
|---|---|---|---|
| `main+0x2C74` (word) | `-b lock` **and** `-block` | 0 | **0** — no effect |
| `main+0x37F31` | `-t 120` | 30 | **120** |
| `main+0x37F35` | `-e 50` | 0 | **50** |
| `0x511DE0` | `-f` | 0 | **1** |
| `0x502898` | `-l` | 1 | **0** |
| `0x513004` | `-p 10` | 200 | **100** (= 1000/10) |
| `0x512C84` / `0x512CA8` | `-h testhost` | 0 / `""` | **1** / **`"testhost"`** |

Separately, `-n 1|3|4` each landed the frontend on the matching DirectPlay dialog
(TCP-address prompt / modem / serial COM-port), and `-c lobby.cfg` set
`main+0x39245=1` and left `0x512DD0` holding the resolved `…/gamedir/online.dll`
path with a live `HMODULE` in `0x512EEC`.

Reproduce:

```bash
tools/tacli launch probe
tools/tacli peek probe '*0x511DE8+0x2C74:2' '*0x511DE8+0x37F31' '0x513004'
tools/tacli stop probe
tools/tacli launch probe --arg=-b --arg=lock --arg=-t --arg=120 --arg=-p --arg=10
tools/tacli peek probe '*0x511DE8+0x2C74:2' '*0x511DE8+0x37F31' '0x513004'
```

(`--arg=-x` with the `=`, or argparse eats the leading dash.)

### The `-b` dead end — do not spend time here again

`-b` was the hope for registry-free skirmish rule presets: eleven battleroom words
`stricmp`'d in sequence at `0x49EF61`…`0x49F04A` — `lock`, `deathends`,
`deathplays`, `deathmatch`, `fixedloc`, `mapping`, `circlos`, `truelos`, `permlos`,
`cheating`, `watching`. It is dead twice over.

1. **It cannot match.** Every other handler opens with `add edi,0x2` to step past
   the switch letters (`0x49F05D`, `0x49F141`, `0x49F197`, `0x49F202`, `0x49F409`).
   The `-b` handler at `0x49EF3A` does not — it goes straight to
   `cmp BYTE PTR [edi],0`. So `edi` still points at `"-b"` / `"-block"`, the
   "argument is empty, pull the next token" branch never fires, and all eleven
   `stricmp`s run against a string that still carries the `-b` prefix. A
   one-instruction bug in the stock binary.
2. **Ten of the eleven bodies are empty anyway.** Only `lock` has one
   (`or WORD PTR [main+0x2C74], 1` at `0x49EF78`); every other word compares equal
   and jumps straight to the loop tail at `0x49F461`. The rules really live in the
   `online.dll` config block — `main+0x2C74` bit 0 is *overwritten* at `0x449DB0`
   from block field `+0xE8` on the lobby path.

So skirmish rules stay on the registry (`SkirmishLineOfSight`, `SkirmishMapping`,
`resolution.md`). Fixing (1) would need a code-cave trampoline and would buy
nothing, because of (2).

### The skirmish key, per player [RUNTIME-VERIFIED 2026-09-01]

`HKCU\Software\Cavedog Entertainment\Total Annihilation\Skirmish` carries six
`REG_DWORD` values for each of `Player0`..`Player9` — the ten seats the engine's own
`Players[]` array has, 0-based:

| Value | Meaning |
|---|---|
| `Player<N>Controller` | 0 = off, 1 = human, 2 = AI |
| `Player<N>Side` | 0 = ARM, 1 = CORE |
| `Player<N>Color` | 0..9 |
| `Player<N>Metal` | **Starting metal**, default 1000 (`0x3E8`) |
| `Player<N>Energy` | **Starting energy**, default 1000 |
| `Player<N>AllyGroup` | 5 on every seat out of the box; unmeasured |

**`Metal` and `Energy` set the storage as well as the level.** Measured on a live
skirmish: `Player0Metal = 5000` gives `fCurrentMetal` 5000 *and* `fMaxMetalStorage`
5000, and both still read 5000 half a minute later — TA does not clamp them back to
what the player's units would hold. That is precisely why the same write into a
*running* game does not stick: there the engine recomputes storage from owned units
every tick and clamps the level to it (`scenario-format.md`, phases C and D). Starting
resources are a launch-time setting or nothing.

`tacli launch --player N:controller[:side[:color[:metal[:energy]]]]` writes them, and an
empty field leaves that key alone. The values are **sticky per instance**, like every
other skirmish setting: what a run does not name, it inherits from the run before.

## `online.dll` — the lobby launch interface [BINARY-VERIFIED + RUNTIME-VERIFIED]

`-c <str>` (handler `0x49F05D` → `0x45B670`) is a **plugin hook**, and the plugin
ships with the game:

1. Zero the 0x150-byte config block at `0x512C80`.
2. If the path buffer `0x512DD0` is empty, build it: `GetModuleFileNameA`, strip
   back past the last `/` or `\`, append `"online.dll"` (`0x4FD4D0`).
3. `LoadLibraryA(that path)` → `0x512EEC`.
4. `ONLGetVersion()` must return **>= 3**, else give up.
5. `ONLLoadConfigFile(<the -c string>, 0x512C80, 0x150)` fills the block;
   its return value becomes the "launched from a lobby" flag at `0x512EE8`
   (`0x45B660()` reads it).

Steam ships `online.dll` next to `TotalA.exe`, and the instance mirror already
symlinks it — a `-c` launch loads it for real (verified: live `HMODULE`).

Known block fields (offsets from `0x512C80`):

| Offset | VA | Meaning |
|---|---|---|
| `+0x00` | `0x512C80` | DirectPlay connection type 1..4 (also set by `-n`); the frontend consumes and clears it |
| `+0x04` | `0x512C84` | "name present" flag (also set by `-h`) |
| `+0x18` | `0x512C98` | fallback name (used when `+0x28` is empty) |
| `+0x28` | `0x512CA8` | name, 63 B stored / 16 B used |
| `+0xE8` | `0x512D68` | → `main+0x2C74` bit 0 at `0x449DB0` — the battleroom `lock` rule |
| `+0xEC` | `0x512D6C` | → `main+0x37EEA` (word) |
| `+0x110` | `0x512D90` | string, also `-n 1:<str>` |

The frontend applies the block at `0x449D60`-`0x44A400`, building game settings by
name (`LUP`, `MAXUNITS`, `ENERGY`, `ALLIES`, `SHARE`, `CONTROL` via `0x4A0570` /
`0x4A1450`), and the whole block is gated on `0x45B660()` — i.e. **network games
only, never skirmish**. `main+0x39245` is the same "lobby launched" flag the
frontend checks at `0x426F74` before playing `1.zrb`.

**Lead, not done:** writing our own `online.dll` would let tacli drive TA's
*multiplayer* setup — rules, players, connection — from a file instead of the
battleroom UI, and `-n 1` already opens the TCP-address prompt directly. That is
the shape of an agent-vs-agent networked test. Out of scope for phase 1.2, which
only needed to know whether the command line could replace the skirmish registry
keys. It cannot.

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
- Binary enumeration 2026-09-01 (switch table decode + `GetOption` call-site sweep) — the authoritative list above.
- Handler tracing + in-process A/B 2026-09-01 (phase 1.2): every switch traced to the
  global it writes and confirmed with `tacli peek` on a live instance.
