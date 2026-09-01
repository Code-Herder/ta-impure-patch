# ProTA

## Summary

ProTA (TAG_Venom) is a competitive-play mod for the original Cavedog `TotalA.exe`. Its
distinguishing feature as a modding artefact is that it ships a **hex-patch set that changes
combat simulation and targeting logic**, not just limits. The canonical machine-readable copy is
`res/prota.ini` in FunkyFr3sh's Total Annihilation Patch Loader, headed `; ProTA 4.5 Patches`
([res/prota.ini:1]). It contains **69 patch sections** plus a 7-key `[Settings]` block.

Two things emerged from reading it against the repo's git history and against `mayhem.ini`:

1. The file in the repo is **not the whole of ProTA 4.5**. Commit `2b30ea1` (pre-2023-10-10)
   carried a machine-generated `prota.ini` of **696 single-byte sections** — evidently a raw byte
   diff of ProTA 4.5's own hex-edited `TotalA.exe`. Commit `34baaa8` ("add organized/commented
   prota patches (by Venom)") replaced it with the curated 69-section version. Comparing the two
   [VERIFIED], the curated file **drops two patches** that were in the shipped exe.
2. ProTA and Total Mayhem share almost the entire corpus: **67 of ProTA's 69 sections appear at
   the identical address in `mayhem.ini`, 64 of them byte-for-byte identical** [VERIFIED].

Addresses below are `file-offset (VA)`. The loader's mapping (`patches.c:318-345`): `.text`
`0x400`→`0x401000` (+0xC00), `.rdata` `0xFAE00`→`0x4FC000` (+0x1200), `.data` `0xFF600`→`0x501000`
(+0x1A00). All disassembly was produced with `objdump -b binary -m i386 -Mintel` from the INI's own
byte rows [VERIFIED].

## The patch set (full table)

| Section (VA) | Name / effect |
|---|---|
| `[Settings]` | `RegistryPath=ProTA`, `ConfigFileName=ProTA.ini`, `Gp3FileName=ProTA.GP3`, `DownloadPath=downloadsP`, `GameVersionString=4.5`, MP version `4`.`5` (`0049E9C0`/`0049E9C9`) — prota.ini:3-21 |
| `00000BD0`–`00000C10` (`004017DA`) | **`init_cloaked` delay**: unit is not cloaked until construction finishes |
| `000029EE` (`004035EE`) | **Acquire targets while attacking a good target in sight range** (no chase needed) |
| `00002D76` (`00403976`) | **Acquire with weapons 1&2 while manually D-gunning ground**; `push 3`→`push 2` ("occupy tertiary only") |
| `0000405F` + `00004198` (`00404C5F`) | Ground-con reclaim sound plays once, not looped |
| `00004370` (`00404F70`) | String-pointer fix `0x501684`→`0x501698`: "Ressurection failed"→"Resurrection failed" |
| `000063B4` (`00406FB4`) | **AI commander orders not reset when attacked** |
| `00007570` (`00408170`) | AI commander waits after 10 units (labs+cons); stock 5 |
| `00007AF0`–`00007B10` (`004086FF`) | **AI stockpile fix** — stop infinite stockpile queueing |
| `00007B24` (`00408724`) | **AI energy management** — shut down any high-`EnergyUse` unit, not just metal makers |
| `00007E20`–`00007EC0` + `00007FA2` (`00408A26`) | **Eliminate target locking** (see case study) |
| `00013EB0`–`00014030` (`00414ABD`) | VTOL reclaim sound, part 1 — play only when the reclaim actually starts (372-byte block relocation) |
| `00014110`–`00014130` (`00414D12`) | VTOL reclaim sound, part 2 — suppress the sound when not reclaiming |
| `0001D14B` + `0001D152` (`0041DD4A`) | Show all players on the final scoreboard, including leavers (and, per the comment, ejected players) |
| `00025AA5` (`004266A5`) | Suppress the DirectX startup warning (`test eax,eax`→`mov al,1`) |
| `0003DB86` (`0043E786`) | Reclaim cursor over any unit; side effect: **commanders anonymous on the minimap** |
| `0003EA13` (`0043F613`) | **Resurrect units can reclaim on command** (reclaim button / `e`); click still resurrects |
| `0004624D` (`00446E46`) | **Allied Victory on by default** — `and word [eax+9Dh],0FFFDh` → `…,0FFFFh`, i.e. the flag bit is never cleared |
| `00095AE7` (`004966E7`) | `\` no longer repeat-last-command (freed for the demo-recorder whiteboard) |
| `00095B76` (`00496776`) | F10 = debug mode (developer mode only) |
| `00095B79` (`00496779`) | Insert = repeat-last-command |
| `000FB780` (`004FC980`) | AI group-5 handler pointer `0x407380`→`0x4086D0` = group 1's, so silos with `builder=1` stockpile (credited to Rahsennor) |
| `000FBA7F` (`004FCC7F`) | `+atm` float `0xC47A0000` (−1000.0f) → `0xD47A0000`: fills metal/energy regardless of storage (credited to N72) |
| `001005D8`, `001005E4` (`00501FD8`, `00501FE4`) | `patch=02` — **undocumented**; also present unnamed in `patches.ini` and `mayhem.ini` |

Dropped from the curated file but present in the raw ProTA 4.5 exe diff [VERIFIED]:
`00002BE0 (004037E0)` = `83 FD 01 7F 04 6A 00 EB 02` (a second acquire-targets site — `mayhem.ini`
documents this exact address as "…in sight range **and needing to chase**"), and
`0000DED6 (0040EAD6)` = `9C 36 01`, the pathfinding `+search` seed as dword `0x1369C` (79516) —
where the community patch's `ChangePathfindingSearch=Yes` sets 66650 (`patches.c:213`).

## Case study: targeting and target-lock elimination

**The acquire-target family.** Stock TA marks a unit's weapons "occupied" whenever it plays the
yellow nanolathe/D-gun SFX, via a thiscall `sub_4898B0(mode)`. ProTA touches this two ways.

*D-gun* (`00403975`): `push 3` → `push 2`, leaving the call intact. Mode 3 occupies all three
weapon slots; mode 2 occupies only the tertiary (the D-gun), so weapons 1 and 2 keep auto-acquiring
while you manually D-gun ground.

*Sight range* (`004035EE`) is cleverer:

```
orig: push 0 ; mov ecx,edi ; call 4898B0     new: cmp ebp,1 ; jg  4035F7
      push 2 ; mov ecx,edi ; call 4898B0          push 0   ; jmp 4035F9
                                                  push 2   ; mov ecx,edi ; call 4898B0
```

Two unconditional calls become one conditional call, in the same nine bytes. [VERIFIED bytes;
`ebp`'s meaning is inferred from the comment.]

`mayhem.ini` extends the family to *building* (`00403D24`), *assist-nanolathing* (`004040B4`),
*reclaiming* (`004047C6`), *repairing* (`00405546`) and *VTOL construct/assist* (`00413E1B`,
`0041441C`) by a different trick — keep `push 3`, but **redirect the call from `0x4898B0` to
`0x489800`**, exactly −0xB0: an existing sibling routine that does the SFX ("yellow change only")
without occupying any weapon [VERIFIED by recomputing every `E8` displacement]. Those sites are
**not** in `prota.ini`.

**Target locking.** In stock TA a weapon stores a 16-bit target handle in its slot record. Once
latched, the retargeting loop never clears it, so the weapon stays "locked" onto that unit even
when a better or more appropriate target is available — it only releases when the target dies or
leaves range. ProTA's `00408A26` patch (11 rows plus a 1-byte jump fixup at `00007FA2`) rewrites
the whole scan head to insert a re-validation:

```
408a92: push edi ; push eax ; push esi
408a95: mov  esi,eax
408a97: call 49abb0            ; NEW: re-validate current target
408a9c: test eax,eax
408aa0: mov  esi,[ebp+0x39]    ; weapon array base
408aa3: jne  408aac
408aa5: mov  WORD PTR [ebx+esi+0x4],0x0   ; NEW: clear the stored target handle
```

`0x49ABB0` is an **existing** engine routine — an INI byte-patch cannot add code, so Venom had to
find a predicate already in the binary and wire it in. The 16 bytes were bought by recompressing
every flag test around it, with no semantic change except where noted:

- `mov eax,[ecx+110h]; test eax,80000000h; je; and eax,300000h; cmp eax,200000h; jne` (23 B)
  → `mov eax,[ecx+112h]; and ax,8020h; cmp ax,8020h; jne` (14 B). Reading the *high word* of the
  same dword makes bit 31 and bit 21 into `0x8000|0x0020`. **Note**: the mask drops the original
  requirement that bit 20 be clear — a small relaxation, not just a size saving.
- `test cl,2; je far; test cl,10h; je far` (18 B) → `and cl,12h; cmp cl,12h; jne far` (12 B).
- `mov ecx,[edx+111h]; mov edx,ecx; shr edx,8; test dl,1` → `mov ecx,[edx+112h]; test cl,1`.
- `mov edx,[esp+24h]; test edx,edx` → `test byte [esp+24h],1` (also a narrowing).
- The weapon-array base moves `eax`→`esi`, freeing `eax` for the new call's return value.

Consistency check: the loop top moves from `00408A57` to `00408A4A`, 13 bytes earlier, and the
backward jump at `00408BA2` is corrected `0xFFFFFEB1`→`0xFFFFFEA4` — exactly −13 [VERIFIED].

**`init_cloaked` delay** (`004017DA`) uses the same budget technique: `mov ecx,eax; shr ecx,0Bh;
test cl,1` becomes `test ah,8` (same bit, 5 bytes saved), and three separate far jumps to
`0040186E` collapse into one shared `jmp` trampoline at `00401808`. The savings pay for
`mov ecx,[esi+104h]; test ecx,ecx; jne 401808` — a new guard on the unit's build-progress field, so
an `init_cloaked` unit stays visible until it is finished.

## AI patches

- **Orders not reset when attacked** (`00406FB4`): `test ch,10h; je 406FF6` → `jmp 406FF6`. Same
  destination, now unconditional. Identical in `patches.ini` (`CommanderOrdersNotReset=Yes`,
  `patches.c:195`) and `mayhem.ini` — a shared community patch.
- **Stockpile fix** (`004086FF`): the three flag tests are reordered and compressed (`shr eax,0Eh`
  lets one `test ah,80h`/`test ah,40h` pair cover bits 29 and 28), buying room for
  `mov ecx,[esi+60h]` … `jecxz 40871E` — the purchase proceeds **only if `[esi+60h]` is zero**,
  i.e. nothing already queued. The 2-byte `jecxz` is chosen precisely to fit. Paired with
  `000FB780`, which points AI group 5 at group 1's handler `0x4086D0` — adjacent to this code,
  a nice cross-check.
- **Energy management** (`00408724`): `mov cl,[eax+22Dh]; test cl,cl; je +53` →
  `cmp byte [eax+1C9h],42h; jl +54; nop`. Both land on `00408781` [VERIFIED]. A metal-maker-only
  boolean becomes a threshold on an energy-use byte, so the AI can shut down targeting facilities
  too. **This is one of only two ProTA-only sections** (`mayhem.ini` has no `00007B24`).
- **AI waiting threshold** (`00408170`): ProTA `0A` (10), Mayhem `0C` (12) — pure tuning.

## Overlap with other mods' patch sets (shared address corpus)

Concretely [VERIFIED by set comparison of the two INIs]:

- `prota.ini`: 69 sections. `mayhem.ini`: 157 sections.
- **67 addresses in common**; 90 Mayhem-only; **2 ProTA-only** (`00007B24` AI energy;
  `0003DB86` cursor-reclaim — and that one is only "ProTA-only" because Mayhem gets it from the
  loader's named `CursorReclaim=Yes` toggle instead of raw bytes, so it is not a real divergence).
- Of the 67 shared, **64 are byte-identical**. The three that differ:

| Address | ProTA | Mayhem |
|---|---|---|
| `0000405F` | `EB 2F 90 90 90 90 C6 47 05 04` | `EB` (Mayhem splits the rest into `[00004060]`) |
| `00007570` | `0A` | `0C` |
| `0001D152` | 15 bytes | 14 bytes (one fewer trailing `90`) |

Only the middle one is a behaviour difference; the comment text is copied verbatim between the two
files. Against the base set (`patches.ini`), ProTA reproduces every named toggle as raw bytes —
`CommanderOrdersNotReset`, `DirectxPopupElimination`, `CursorReclaim`,
`DisableKeyLastCommandRepeat`, `EnableF10Debug`, `IncreaseAtmToFillResources`, plus the two
undocumented `.data` bytes — but **omits `ChangePathfindingSearch`**, which ProTA's own 4.5 exe did
carry (at 79516, not the community 66650).

This is emphatically a **shared community corpus**, not independent reversing: Venom is credited in
`mayhem.ini`'s lineage and, per `tdraw.txt`, is a principal tdraw.dll contributor.

## Distribution & binaries

ProTA historically shipped a **hex-edited `TotalA.exe`** — the Patch Loader's README says it was
"Made to replace the hex edited TotalA.exe in the Total Annihilation community patch", and the
696-single-byte pre-curation `prota.ini` is that exe's byte diff. Modern ProTA is the mainline
consumer of the Patch Loader plus the community DLLs.

There is **no `pdraw.dll`**. The Patch Loader is a **`dplayx.dll` proxy** forwarding all exports to
`tplayx.dll` (`exports.def:1-12`); `DllMain` verifies the 3.1 exe by a 10-byte signature at
`+0x10000`, applies the INI, sets a marker byte at `0x00401064` to tell `tdraw.dll` the proxy is
live, then `LoadLibrary("tdraw.dll")` and repoints two `DirectDrawCreate` call sites (`0x0047BFA2`,
`0x004B55FB`). Per TADR's `tdraw.txt`, **ProTA uses the unmodified name `tdraw.dll`** — it is the
mainline build (`tdraw-prota.zip`, "all features enabled"); Mayhem renames it `mdraw.dll`,
Escalation `taesc.dll`, Zero `zdraw.dll`. TADR carries `src/DDraw/config_prota.h` and
`src/Recorder/dist/prota/tplayx.dll`, and lists "ProTA 4.6 / ProTA.ini / tdraw.dll".

Two features the brief attributes to ProTA — **build-menu rotation overlays** and **allied
queued-build overlays** — are `tdraw.dll` features contributed by TAG_Venom (`tdraw.txt:81, 725,
768-775`), not `prota.ini` hex patches.

I could **not obtain any ProTA binary**: no ProTA folder under `files.tauniverse.com/files/ta/mods/`;
it is a TAF featured mod whose client "pulls down each mod on demand", and `content.taforever.com`
returns 403 anonymously. So **no `sha256sum`/`strings`/`objdump -p` was run**, and the reported
680,448-byte `tdraw.dll` size is [CLAIMED], unverified here.

## Multiplayer version enforcement

Three independent layers [VERIFIED from the INI, loader source, and `tdraw.txt`]:

1. **Battleroom version bytes.** `MultiplayerVersionMajor/Minor` write `0x0049E9C0` and
   `0x0049E9C9` (`patches.c:163,179`); ProTA sets 4/5, Mayhem 10/9, base OTA 3/1. The INI comment
   states plainly: "all players must match" (`prota.ini:19`). This is the hard gate — a mismatched
   client cannot join.
2. **Loader-side integrity refusals.** `DllMain` aborts on an unrecognised exe signature, on mixed
   community-patch files (`DDRAW.dll` string vs MP version 3.1), and `patches_setbytes` rejects any
   two patches whose byte ranges overlap, reporting the conflicting addresses (`patches.c:27-56`).
3. **In-game anticheat hashing.** `tdraw.dll` hashes `TotalA.exe` in background threads and logs
   cheat warnings to `tdrawlog.txt`; `tdraw.txt:173` is explicit that a single mismatched patch
   byte between players "will trigger an in-game cheat warning". Battleroom commands `.exereport`,
   `.tdreport`, `.tpreport`, `.gp3report`, `.crcreport` print each player's CRC32 for `totala.exe`,
   `tdraw.dll`, `tplayx.dll` and the `.GP3`, so mismatches are visible before launch.

Because the loader patches memory at load time rather than shipping a modified exe, players'
`TotalA.exe` files stay byte-identical on disk and (3) reduces to "everyone is running the same
INI". That is the real reason the Patch Loader exists.

## Sources

- `res/prota.ini` (69 sections; `; ProTA 4.5 Patches`), `res/mayhem.ini`, `res/patches.ini`,
  `patches.c`, `dllmain.c`, `exports.def`, `README.md` —
  https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader
- Git history: commit `2b30ea1` (raw 696-byte-section `prota.ini`) vs `34baaa8`
  "add organized/commented prota patches (by Venom)", 2023-10-10.
- `src/DDraw/tdraw.txt` and `src/DDraw/config_prota.h` —
  https://github.com/tanvanman/TADR
- https://www.taforever.com/ (ProTA listed as a featured mod) and
  https://www.taforever.com/mod_standards
- https://files.tauniverse.com/files/ta/mods/ (no ProTA folder)
- Disassembly performed locally with `objdump -D -b binary -m i386 -Mintel` on the byte rows quoted
  in the INIs. No `TotalA.exe` was obtained or executed.
