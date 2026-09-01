# Total Annihilation: Escalation

Deep dive, 2026-08-31. Subject: **TA:ESC GOLD 10.2.0** (WotanESC & The Registered One),
`taesc.tauniverse.com`. Scope: original Cavedog `TotalA.exe`.
Evidence: a first-hand teardown of the shipped 334 MB distribution.

## Summary & verdict

**TA:ESC does not write engine patches. It consumes the community's, and implements every
headline gameplay feature in data and COB scripts.** Three findings, each independently verified:

1. **The engine DLL is not theirs, and is not even rebuilt.** `TAESC.dll` shipped in GOLD 10.2.0 is
   **byte-identical** (908,288 bytes, sha256 `61783aca…`) to `tdraw.dll` inside upstream
   **`tdraw-escalation.zip` from `tanvanman/TADR` release `v2026.8.6`**. Not a fork, not a rebuild —
   the same file, renamed. Its own version resource says `OriginalFilename=tdraw.dll`,
   `CompanyName=TA Community`, `ProductName=TA PATCH`, `FileVersion=2026.8.6.0-esc`. [VERIFIED]
2. **The shipped exe is a community-patched exe with ESC branding hex-edited in.** Its ESC-specific
   edits are the standard import rename plus data-path/registry renames, applied at exactly the file
   offsets FunkyFr3sh's Patch Loader documents for that purpose. [VERIFIED]
3. **Shields are COB scripts driving the stock 1997 `set ARMORED` opcode and the stock
   `damagemodifier` FBI key.** There is no engine shield anywhere in ESC's stack. TADR *does* ship an
   engine shield (`ShieldRange`), and TA:ESC uses it **zero** times. [VERIFIED]

The user's premise — that shields/teleport/upgrades are engine features added by patching the exe —
is wrong on both halves: they are not engine features, and TA:ESC did not patch the exe to get them.
The mod's own readme and credits say so, and the binaries agree.

## What TA:ESC actually adds

Content and design, at very large scale: 250+ new units, a full Level 3 tier plus "Experimental"
units, rebalance, models/textures/GUI, AI, maps, music. The headline "engine-like" features are all
data/script constructs:

| Feature | Actual mechanism | Evidence |
|---|---|---|
| Deflective shields | COB `Detect` polling loop → `set ARMORED,1`; FBI `damagemodifier=0.25` (4× effective health) | [VERIFIED] disassembly |
| Mass teleport / galactic gates | COB `Teleport` script on gate units, plus `OpenYard`/`CloseYard` | [VERIFIED] COB script table |
| Upgradable buildings | A cloaked, stealthed, `HideDamage=1` companion **unit** built onto the parent (`corfus_upgrade.fbi`: `Init_cloaked=1; Stealth=1; DamageModifier=0`) | [VERIFIED] FBI |
| Adjacency bonuses | Same detect-and-modify script pattern over neighbouring buildings | [VERIFIED] changelog + script names |
| Multi-unit air transports | COB transport scripting (credits thank Zwzsg for "especially transports") | [VERIFIED] credits |
| Surfacable nuclear subs | **TADR's** `surfacefire=1` weapon TDF flag — 12 weapon entries incl. "Sub Starburst Missile" | [VERIFIED] |
| Level 3 tier | Pure unit data + build menus | [VERIFIED] |

Its `TAESC.ini` is the community patch's `Settings.ini`, rebranded: every entry is a TADR knob
(`UnitLimit=1000`, `AISearchMapEntries=66650`, `SfxLimit=20480`, `UnitType/WeaponType=16000`,
`FullScreenMinimap`, `MegamapDither`) documented as "TA v3.1 default is X / TA Escalation default
is Y". **There is not one shield, teleport, upgrade or adjacency setting in it.** [VERIFIED]

## Binary inspection (edraw/taesc.dll, the shipped exe)

Distribution: `TAESC_GOLD_10_2_0_FULL.rar`, **350,859,995 bytes** (matches the site's stated size),
sha256 `a9873e551d7fa72ad2f74ca37d8bbc7c873043d178979c842bbf8ed667eea2c3`; 73 files,
378,497,183 bytes uncompressed. Static analysis only; nothing executed.

**Test 1 — the DLLs.** The user's brief said `taesc.dll`/`edraw.dll` are 680,448 bytes and
byte-identical to `tdraw.dll`. **Same size, but not identical — 26 bytes differ**, and what those
26 bytes are matters more than the count:

| File | Size | sha256 |
|---|---|---|
| `edraw.dll` (2022 drop-in) | 680,448 | `af91de813c6d4ebe9822167f92f6af4c367311e9974b369dde71b7f0190250e5` |
| `taesc.dll` (2022 drop-in) | 680,448 | identical to `edraw.dll` (cmp exit 0) |
| `tdraw.dll` (OTA drop-in) | 680,448 | `9ce51279fa236ee35b6d33a626548e313361e16f4bc5a8d2b063e0716dd1d0d7` |

The 26 differing bytes are: the PE `TimeDateStamp` (`0x63541229` = 2022-10-22 15:54:17Z vs
`0x63541366` = 15:59:34Z — **317 seconds apart**), three debug-directory timestamps, the 16-byte
RSDS PDB GUID, and **one letter** in `…\TA-ReImagined\DDraw\Output\edraw.pdb` and in the export
directory name `edraw.dll`. Zero code or data differences. Their version resources are identical
(`CompanyName=vThaldren Developments`, `OriginalFilename=idraw.dll`). This is one source tree built
twice in one session under two output names. [VERIFIED]

GOLD 10.2.0 ships a newer, larger `TAESC.dll` (908,288 bytes) which is **byte-identical** to
upstream `tdraw-escalation.zip`. Upstream's build matrix (`.github/workflows/compile.yml`) compiles
five flavours from one tree via `/p:TDRAW_CONFIG=TDRAW_CONFIG_ESCALATION`, and `ddraw.rc` stamps
`VERSION_TAG "-esc"`. Features are **compile-time**, not runtime-detected; the only trace of
identity is a debug string, and ESC's DLL contains `Process Attached.  config=escalation`. [VERIFIED]

Every other DLL is likewise community-authored: `eplayx.dll` (298,496) is the TA Demo Recorder
(`CompanyName=Swedish Yankspankers & TA Universe community`, Delphi paths
`F:\TA\TADR\SVN\trunk\src\Recorder\*.pas`), differing from `tplayx.dll` in **67 bytes / 31 runs** —
`.tad`→`.ted`, `Software\OTA\TA Demo`→`Software\TA Esc\TA Demo`, two opcode flips, and version-
resource bookkeeping. `ddraw.dll` (411,648) is unmodified cnc-ddraw 7.1.0.0 (FunkyFr3sh).
`emusi.dll` (124,928) is Plobex/Damian Woroch's "TA Music Player". [VERIFIED]

**Test 2 — the exe.** `TotalA.exe`, 1,178,624 bytes, sha256 `06d9e87f…`. The import rename is
present and is the standard same-length trick: `DDRAW.dll`→**`TAESC.dll`** at file offset
`0x0FE418`, `WINMM`→`EMUSI.dll`, `DPLAYX`→`EPLAYX.dll` (9/9/10 chars respectively — all preserved).

But that is *not* all they did. Beyond the import table, every data path is renamed **in place at
the identical offset**, and those offsets are exactly the ones FunkyFr3sh's Patch Loader publishes
for rebranding a mod:

| Community exe | TA:ESC exe | Offset | Loader INI key |
|---|---|---|---|
| `gamedata` | `gamedatE` | `0x101960` | — |
| `Weapons` | `weaponE` | `0x101F2C` | — |
| `unitpics` | `unitpicE` | `0x1045E4` | — |
| `ai\default.txt` | `aE\default.txt` | `0x105908` | — |
| `rev%s.GP3` | `TAE%s.GP3` | `0x100ECC` | `Gp3FileName` (`00100ECC`) |
| `download` | `downloadsE` | `0x101D30` | `DownloadPath` (`00101D30`) |
| `%s\ta.ini` | `%s\taesc.ini` | `0x107EA0` | `ConfigFileName` (`00107EA3`) |
| `Software\Cavedog Entertainment` | `Software\TA Esc` | `0x10C3F4` | `RegistryPath` (`0010C3FD`) |

The renamed folders match the HPI archives exactly (`unitsE/`, `weaponE/`, `gamedatE/`, `guiE/`,
`aE/`). The exe is also flagged **`IMAGE_FILE_LARGE_ADDRESS_AWARE`** (Characteristics `0x012B` vs
`0x010B`) and has a recomputed PE checksum. [VERIFIED]

Against a 2013-vintage community-patch 3.9.02 exe, ESC's differs by **3,283 bytes in 533 runs**,
including real instruction-level restructuring. That is *not* ESC-original engine work: ESC's exe
contains **61 of 69** ProTA patch sites and **112 of 157** Total Mayhem patch sites already applied
on disk, where the 2013 reference exe has only **9 of each**. Those patches are authored by
TAG_Venom, gamma, Rahsennor and FunkyFr3sh. [VERIFIED]

1,159 differing bytes are positively attributed to those published community patch tables; 1,725
bytes in 117 `.text` runs remain unattributed — but only because I lack patch tables for the newer
ProTA/TADR generations, not because there is evidence they are ESC's. **No ESC-original engine
change was identified anywhere in the exe.** [VERIFIED, with the stated attribution gap]

Corroboration: the readme's own "ENGINE ENHANCEMENTS" list is verbatim the ProTA/Mayhem patch-loader
feature set ("Eliminate target locking", "Allow weapons to acquire targets while reclaiming/
capturing/repairing…", "Enable debug mode via F10", "No-CD check"), which is precisely what the
byte-level patch-site matches predict.

## How the shields really work

The generator (`armshgen.fbi`, "Aegis") has **no weapon at all**. It carries `damagemodifier=0.25`
and `HealTime=1`. 265 FBIs carry `damagemodifier`, overwhelmingly `0.25` — i.e. 4× effective health,
matching the in-game text "Increases Unit Health (4x)".

Disassembling `armshgen.cob` (I calibrated a COB disassembler against the matched `zzz.bos`/`zzz.cob`
source/binary pair shipped in `TADEMO.ufo`, confirming `GET`=`0x10042000` with the canonical Cavedog
constant table, `BUILD_PERCENT_LEFT`=17):

- `Detect()` opens with `sleep Rand(500,5000)` — a randomised poll.
- It reads two extended `GET` constants (69, 70 — the unit-ID bounds), then loops over unit IDs
  calling `get UNIT_XZ(id)` (9) and `get UNIT_HEIGHT(id)` (11) with squared-distance constants
  (`8192000`, `8601600`) — a brute-force proximity scan.
- The state change is the three-word sequence `PUSH_CONST 20; PUSH_CONST 0|1; 0x10082000`, i.e.
  **`set ARMORED, 0` / `set ARMORED, 1`** — the stock 1997 COB opcode.

Census over 550 COB scripts: **216 contain a `Detect` script; 390 contain `set ARMORED` across 969
call sites.** Even `armsolar.cob` (a T1 solar collector) runs one. [VERIFIED]

This explains the changelog perfectly: *"Increased shield detect timings for Level 2 units and above
to reduce lag"*, *"Modified shield script timings for lower tiered units (longer sleep delays between
detection)"*, *"only those who are shielded run checks"*, *"Fixed all armored units when shield fails
OR when turned back on"*. It also explains the FAQ's requirement that all players share the same max
unit count: the scan iterates the unit-ID table, whose size is `UnitLimit`, so a mismatch desyncs —
exactly why *"Teleporters … are scripted specifically to each version of TA:ESC"*.

**Crucially, TADR ships a real engine shield** — `UnitInfoExpand.pas` parses a new FBI key
`ShieldRange`, `UnitSearchHandlers.pas` computes `ShieldedBy` via an engine unit search, and
`Broadcast_ExtraUnitState` network-syncs it, with a `SHIELDICON` sidedata tag and GAF overlay.
**TA:ESC uses `ShieldRange` zero times.** Its shields predate and bypass the engine feature. [VERIFIED]

## Engine features consumed vs. authored

**Authored by TA:ESC:** unit data (FBI/TDF), COB scripts, models, textures, GUI, AI, maps, balance.
Nothing else.

**Consumed from the community patch** — used, not written: `surfacefire=1` (12 weapons — this *is*
the "surfacable nuclear subs"), `nottoair` (73 uses), `veterancythresholds` +
`veterancyaccuracybuffrate` (146 FBIs each), plus the whole TADR limit/UI stack via `TAESC.ini`.

**Written by others *for* TA:ESC on request.** Upstream `tdraw.txt` gates four features to the
escalation build — share-abuse guard, repair-rate exploit fix, aircraft wrecks falling, 32-tile
off-map AA — and records: *"Fix repair rate exploit … (Escalation only, **requested by Wotan**,
thanks RdKL)"*, and thanks *"Gamma and Wotan for further testing and **feature suggestions**"*. The
ESC changelog's own "**NEW DLL UPDATES**" section lists exactly these as things they received.
WotanESC specifies; the TADR maintainer implements. [VERIFIED]

TA:ESC states this itself. The readme: *"Leverage multiple **community driven** UI and engine
improvements"*; *"the TA 3.9.02 Community patch … is not required or needed for ESC, however
(3.9.0.2 enhancements are already part of ESC)"*. Its credits split the work explicitly under
**"Engine/Back End"**: *"Admiral_94 for multiple engine enhancements to totala.exe!"*, *"Axle for
DLL engine enhancements"*, *"FunkyFr3sh for superb extended ddraw and dplayx support"*, *"R1ME…"*,
*"xpoy for awesome ddraw dll based updates including megamap, pathfinding…"*, *"Xon for critical
scripting extensions"*. [VERIFIED]

This also settles a prior open question: **Zwzsg is credited for scripting, not exe hacking** —
*"Zwzsg for advanced (and often experimental) scripting techniques (especially with transports)"*.

## Project status, team, distribution

Active and the most prominent TA mod. GOLD 10.2.0 released **2026-08-08**; 10.1.0 on 2026-02-13.
Site `taesc.tauniverse.com` is public (Cloudflare-fronted but not gated); the linked
`tauniverse.com/forum` is members-only and was not accessed.

Team per the readme: **Wotan** and **The Registered One (TRO)** create and maintain it, with
**WaRLOrD** (buildpics, menus, missions, tilesets) and **Losparkeros** (renders, art, guides);
**TAfan97** on AI. Lineage: a 2008-08-08 merger of **Talon** (TRO) and **TAWP** (Wotan), influenced
by Uberhack and TA:Devolution. Copyright "© 2008-2026 The Registered One and Wotan".

Distribution is a manual-install RAR (three `Step_N` folders) containing HPI-format `.ufo`/`.gp3`
archives, the four DLLs, `TotalA.exe` + `Viewer.exe`, 17 MP3s and cnc-ddraw shaders. Requires a
licensed TA + Core Contingency, minimum 1024×768; incompatible with other mods.

## Confidence & gaps

- All binary claims are [VERIFIED] by commands run here (sha256, `cmp`, byte-diff, `objdump -p`,
  PE parsing, HPI extraction, COB disassembly). No downloaded binary was executed.
- The upstream identity of `TAESC.dll` was confirmed against GitHub release assets (`cmp` exit 0).
- **Gap:** 1,725 bytes across 117 `.text` runs in the exe are unattributed. I had ProTA 4.5 and
  Mayhem patch tables only, and no stock TA 3.1 exe for a three-way diff. I found no positive
  evidence of ESC-original engine code, but cannot prove a negative for those bytes.
- **Gap:** the COB disassembler's opcode names are calibrated for the opcodes actually observed;
  extended `GET` constants 69/70/73/74 are inferred as unit-ID/ownership queries from usage, not
  from a published table. The `set ARMORED` finding does not depend on that inference.
- The readme lists sections 11–12 in its contents but the headed sections are absent from the file.

## Sources

Primary (first-hand teardown): `TAESC_GOLD_10_2_0_FULL.rar` — `ESC_READ_ME.txt`,
`GOLD_10_2_0.txt` (13,710-line changelog), `TAESC.ini`, `TotalA.exe`, `TAESC.dll`, `eplayx.dll`,
`emusi.dll`, `ddraw.dll`, and the `.ufo`/`.gp3` HPI archives (550 COB, 549 FBI, 12 TDF extracted).

- <https://taesc.tauniverse.com/> — intro, FAQ, downloads pages
- <https://github.com/tanvanman/TADR> — release `v2026.8.6` assets, `src/DDraw/tdraw.txt`,
  `config_escalation.h`, `.github/workflows/compile.yml`, `ddraw.rc`,
  `src/Recorder/plugins/UnitInfoExpand.pas`, `UnitSearchHandlers.pas`
- <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader> — `res/prota.ini`, `res/mayhem.ini`
- <https://github.com/gammata/TA-Unofficial-Patch-Install> — OTA/ESCALATION drop-ins
- <https://github.com/ioma8/totala-re> — `hpi_parser.py`, used for extraction
