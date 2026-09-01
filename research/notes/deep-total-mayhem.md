# Total Mayhem

## Summary

Total Mayhem (Mayhem Inc., lead: **gamma**) is a 200+ unit rebalance/expansion for OTA. Current release
**11.3.0, 20 June 2024**, `TotalM1130.zip`, 26,035,480 bytes, site-quoted CRC32 `6C73C0C1`,
sha256 `f485e8950a4b71940f414159ab6375a3cc32f35c2e8a962918e5984d801363af` [VERIFIED — downloaded].

Its engine work splits into two eras:

* **≤ 11.1 (e.g. 8.1.5, 2020):** ships a **pre-hex-edited `TotalA.exe`** plus a renamed copy of the
  community TA Patch DLL called `mdraw.dll`. [VERIFIED: the 8.1.5 exe contains `guiM`, `may%s.GP3`,
  `unitsM`, `WeaponM`, `unitpicM`, `%s\totalm.ini`, `Software\TotalM` at the expected offsets.]
* **11.2 / 11.3 (2024):** ships a **stock, unmodified `TotalA.exe`** (`v3.1`, `guis`, `rev%s.GP3`,
  `Software\Cavedog Entertainment` all intact) and applies every patch **at load time** via FunkyFr3sh's
  *Total Annihilation Patch Loader*, shipped as **`dplayx.dll` (34,816 bytes)** with the patch INI
  embedded as RCDATA resource 1000. [VERIFIED — extracted the INI from the DLL: header line
  `; Mayhem 11.3.0 Patches`, 581 lines, 160 `[offset]` sections.]

The `res/mayhem.ini` in `github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader` is the **10.9.2**
snapshot: 951 lines, **158** patch sections (the extra lines are `;row` before/after hex dumps).
Offsets in both are **file offsets**; VA = file + `0x400C00` for `.text`, + `0x401A00` for `.rdata`/`.data`.

## The patch set (categorised, with notable addresses)

**1. Mod identity & per-mod asset redirection** (`[Settings]` + string pokes). All verified against the
stock exe's string table:

| file off | stock string | becomes |
|---|---|---|
| `0x00100E23` | `guis` | `guiM` |
| `0x00100ECC` / `0x00100ED8` | `rev%s.GP3` + `"31"` | `may%s.GP3` + `"hem"` → **`mayhem.gp3`** |
| `0x00101F25` (10.9.2 only) | `units` | `unitsM` |
| `0x00101F32`, `0x00101F3A` | `Weapons`, `Weapons\*.tdf` | `WeaponM`, `WeaponM\*.tdf` |
| `0x001045EB` | `unitpics` | `unitpicM` |
| `0x00101D30` | `download` | `downloadsM` |
| `0x00107EA3` | `%s\totala.ini` | `mayhem.ini` |
| `0x0010C3FD`, `0x001084B8` | `Software\Cavedog Entertainment` | `Software\TotalM` |

The `"31"`→`"hem"` poke is the neatest thing in the file: stock `rev%s.GP3` + literal `"31"` yields
`rev31.gp3`; Mayhem makes it `may%s.GP3` + `"hem"`.

**2. Weapon target-acquisition unlocking (~13 patches, the largest single group).** Everywhere OTA calls
`push 3; call SetOccupyClass` inside a task handler, Mayhem changes the pushed occupy class and/or
retargets the call so weapons stay live while the unit is busy: while building (`0x00403D24`),
assist-nanolathing (`0x004040B4`), reclaiming a unit (`0x004047C6`), repairing (`0x00405546`),
DGUNning ground (`0x00403975`, `push 3`→`push 2`), plus six VTOL equivalents (`0x00413E1B`,
`0x0041441C`, `0x00414831`, `0x00414DB1`, `0x00414FAE`, `0x0041582C`), and two "acquire while already
attacking" fixes (`0x004035EE`, `0x004037E0`).

**3. AI** — see below.

**4. Bug fixes.** Repair pads ignoring aircraft that are switched off (`0x00402608`); reclaim sound
looping forever on ground cons (`0x00404C5F`) and firing when a VTOL is *not* reclaiming
(`0x00414ABC` / `0x00414D12`, ~25 patch rows — the single biggest rewrite in the file); Cavedog's
`"Ressurection failed"` typo (`0x00404F6F` — repointed from `0x00501684` to the correctly-spelled
`0x00501698`, which also exists in the stock binary); Ctrl-Z crash on many unit types (`0x0048B24F`+);
`init_cloaked` units cloaking before completion (`0x004017F8`); setSFXoccupy on ascending submerged
units (`0x0043DB93`).

**5. Rules/UI.** Allied Victory on by default (`0x00446E46`); full end-game scoreboard for players who
left (`0x0041DD4A`); reclaim of any unit unless `Commander=1` (`0x00088D8B`, moving the tested bitfield
byte from `+0x245`/`ah&0x10` to `+0x246`/`ah&0x04`); resurrection units can reclaim on command
(`0x0003EA13`); keyboard remaps at `0x00095AE7`/`0x00095B76`/`0x00095B79`; DirectX popup killed
(`0x004266A5`); `+atm` fills storage (`0x000FBA7F`, float `-1000.0` → `-4.3e12`).

**6. Unknown.** The file honestly labels `0x000FB239`, `0x000FB255`, `0x000FB271`, `0x001005D8`,
`0x001005E4` as `STILL UNKNOWN`. 11.3.0 adds two identified ones: click-snap radii for metal patches
(`0x00101F0A` = `03 03`) and reclaimable features (`0x00101F12` = `01 01`), poked into `.rdata` padding
and read by `tdraw.dll`.

## Case study: the teleport order button & dormant engine logic

**The claim is true, and stronger than reported.** Cavedog left the *entire* teleport feature in the
1997 binary — tag parser, order table entry, handler function, status string, cursor — and omitted only
the GUI dispatch that can select it. All of the following is [VERIFIED] by static analysis of the stock
`TotalA.exe` shipped with Mayhem 11.3.0 (version string `v3.1`, stock registry path).

1. **FBI tag.** `teleporter` lives at VA `0x00503BA4`, inside the boolean-tag run
   `…shootme, hidedamage, teleporter, istargetingupgrade, isairbase, zbuffer…`. The parser at
   **VA `0x0042C61D`** does:
   `push 0x503BA4; call 0x4C46C0; and eax,1; and dh,0xDF; shl eax,0xD; or edx,eax; mov [ebp+0x241],edx`.
   That is **`UnitDefStruct+0x241`, bit 13 = `0x2000`** — exactly the sibling finding, confirmed at the
   instruction level.
2. **Order table.** At file `0xFB4E8` (VA `0x004FCEE8`) there is a 25-byte-stride task descriptor array
   `{ char* gerund; void* handler; void* handler2; byte flags[9]; char* name; }`. Entry **12 (0x0C)**
   is `name="Teleport"` (`0x00501408`), `gerund="Teleporting"` (`0x00501414`),
   **handler = `0x00406AA0`**, aux = `0x00439740`. (Layout validated against the ini's own comments:
   e.g. entry 19 `ReclaimUnit` handler `0x00404730` contains the ini's `0x004047C6` reclaim fix.)
3. **The handler is real, complete code.** Disassembling `0x00406AA0`: it reads the unit's world
   position (`[esi+0x6A/0x6E/0x72]`), reads six bounds from the unit-def (`[unitdef+0x15E … +0x172]`) to
   form the "yard" box, walks the global unit list (`[0x00511DE8]+0x14357 … +0x1435B`), rejects units
   outside the box and itself (`cmp esi,ebx; je`), then for each contained unit computes a delta from
   the teleport target and calls `0x00471FD0(&unit->pos, …, 0x1E, 5)`. It is referenced from **exactly
   one place: the order table** (`0xFB618`) — i.e. reachable only if something issues order 0x0C.
4. **Even the button name exists.** `"TELEPORT"` at VA `0x00505324` sits between `QMOVE` and
   `ATTACKSPECIAL` in the GUI-order name table, and the stock exe *already* references it at
   **VA `0x0043F817`** inside a per-unit command-name dispatch.
5. **What Rahsennor added.** The gap was the button click handler at **VA `0x00419CFE`**, which in stock
   compares the clicked gadget's name only against `"ATTACK"` (`0x0050270C`) and then hard-codes
   `mov byte [0x00511DE8+0x2CC3], 3`. The patch NOPs a redundant 8-byte call (`push edx/eax/esi; call
   0x0049FED0`), reclaiming 4 bytes, and rewrites the block as: compare `"ATTACK"` → `mov cl,3`;
   if no match compare **`"TELEPORT"` (`0x00505324`)** → `mov cl,0x0B`; then the shared tail
   `mov [eax+0x2CC3], cl` instead of an immediate. Roughly **40 patched bytes** to surface a feature
   Cavedog fully implemented and never exposed. (Note the pending-order byte at `+0x2CC3` is a
   *cursor-mode* enum — `3` = attack, `0x0B` = teleport — distinct from the `0x0C` order-table index.)

## Widening the build interface

**It is 12, not 10, and it is not an exe patch.** The OTA→10.9 changelog shipped in the zip says
"build menu gui changed to standard extended with **12 buttons per page**". Extracting `guiM/` from
`mayhem.gp3` (HPI v1, unencrypted — `headerKey == 0`) confirms it: e.g. `ARMAAP1.GUI` has
`totalgadgets=28`, a `HEADER` panel of `width=128; height=640`, and **twelve** 64×64 build gadgets at
`xpos` ∈ {0,64} × `ypos` ∈ {0,64,128,192,256,320}, i.e. a **2 × 6** grid. [VERIFIED]

So TA's build grid was never hard-coded at six: the number and placement of build buttons is data, read
from the per-unit `.GUI` gadget list (the engine also handles paging via the `%s%d.GUI` name format,
which exists in stock). Widening it means (a) authoring taller `.GUI` files, (b) redirecting the folder
(`guis`→`guiM`), and (c) guaranteeing vertical room — hence `DisplayModeMinHeight768=TRUE` in Mayhem's
config and the 640-pixel panel. **No patch in `mayhem.ini` touches the build grid** — I grepped all
158/160 sections. The "10" in the notes is most likely a different GUI set (2 × 5, e.g. an
`extended` variant sized for a shorter panel), not Mayhem.

## AI modified in the binary

This is the general lesson: **TA's AI is substantially tunable by exe patching, not only via AI profile
data files.**

* **Income by difficulty.** Difficulty selects between two doubles via `je`/`jne` around
  `fmul qword ptr [const]`. Mayhem flips the branch polarity at seven sites
  (`0x0000084C`, `0x00000909`, `0x00000995`, `0x00000A0D`, `0x00000A93`, `0x00000B09`, `0x00000B69`;
  `74`↔`75`, `84`↔`85`) so medium takes the old hard path (1.0×), then rewrites the constant at
  `0x000FB280` (VA `0x004FCC80`) from `-0.7` to **`-2.0`** — hard becomes 2.0×.
* **Reclaim income by difficulty.** Same trick: polarity flips at `0x00022CD5` / `0x00022D2F`, constant
  at `0x000FBE40` (VA `0x004FD840`) `-0.7` → `-2.0`.
  *(Discrepancy: the 10.9.2 comments claim 1.5×; the shipped bytes are 2.0×. The 11.3.0 comment was
  corrected to "2.0x income for Hard setting".)*
* **Nukes/antinukes.** `[0x000FB780] patch=D0 86` rewrites the **first entry of an AI task-handler
  pointer table** (VA `0x004FD180`) from `0x00407380` to `0x004086D0` — the routine containing the
  stockpile-purchase logic at `0x004086FF`. So "AI can use nukes" is literally one repointed vtable slot.
* **Stockpile.** `0x004086FF` rewritten (3 rows) so the AI stops infinitely re-queueing stockpile
  weapons when resources dip; silos need `builder=1`.
* **Commander idling.** `0x0040816E`: `cmp eax,5` → `cmp eax,0x0C` — 12 labs/cons before the AI
  commander waits. *(Comment says 10; shipped byte is `0C` = 12.)*
* **Other:** AI orders no longer reset when attacked (`0x00406FB4`, `jz`→`jmp`); AI ground squads stop
  attacking submerged targets (`0x00407223`, ~12 rows); target-lock elimination (`0x00408A26`,
  ~14 rows); 11.3.0 adds "AI turns off energy-hungry buildings when low on energy" reading the
  `EnergyUse` tag (`0x00408724`).

## Other engine changes

* **`setSFXoccupy` for submerged structures** — `0x00485C69`, ~9 rows: computes the structure's
  height against sea level and forces occupy class `3` (submerged) for *created structures*, matching
  mobile behaviour. Paired with the ascent fix at `0x0043DB93`. Both are prerequisites for the AI
  submerged-target patch.
* **`healtime`** — `0x0048AF3D`, ~11 rows: prevents self-healing before construction completes, allows
  any `healtime > 0`, and changes the rate divisor (`sar edx,4` → `sar edx,2`) for variable rates.
* **Hold-position guard fix** — `0x00405D57`, 5 rows: guarding units on Hold Position no longer chase
  attackers.
* **Pathfinding search depth** — `0x0040EAD3`: `mov [esi+0x48], 0x535` (1333) → `0x1045A` (**66650**).
  Exposed to users as `AISearchMapEntries = 66650` in `mayhem.ini`.
* **Off-map AA** — [CLAIMED] I found no corroboration in `mayhem.ini`, either shipped changelog, or the
  site. Treat as unverified.

## Binary inspection of mdraw.dll

**STATIC ONLY — nothing was executed.**

| file | source | size | sha256 |
|---|---|---|---|
| `mdraw.dll` | TotalM815.zip (2020-01-30) | **423,424** | `3a2708370a8935cab168f23509a515e04c7da559a37f495dcd331aac66cb18bf` |
| `tdraw.dll` | TotalM1130.zip (2024-03-30) | **628,224** | `b8ea362e1bf3e47f00b43e929cd2689746b89338dbb9a7cf0f4b8b6ea2e73a9d` |

`mdraw.dll` — PE32, ImageBase `0x10000000`, MSVC linker 14.24, 22 exports (the DirectDraw surface:
`DirectDrawCreate`, `DirectDrawCreateEx`, `AcquireDDThreadLock`, `SetAppCompatData`, …), imports only
`IMM32/USER32/KERNEL32/GDI32/ADVAPI32`.

**KEY TEST result: it is the community TA Patch DLL, but not byte-identical to `tdraw.dll`.**
Its version resource reads `CompanyName = "Swedish Yankspankers"`, `InternalName = "Swedish Eye"`,
`ProductName = "TA PATCH 3.9.2.0"`, `FileVersion = 3.9.2.0`, and — decisively —
**`OriginalFilename = tdraw.dll`**. It also stores settings under `Software\TA Patch`. However its
**PE export-directory module name is `mdraw.dll`**, i.e. it was linked/renamed as `mdraw.dll`, so it
cannot be a byte-identical copy of any file named `tdraw.dll`. Conclusion: **Mayhem is a consumer of the
community patch, rebranded — not independent DLL work.** By 11.3.0 the rebranding was dropped and the
file ships under its real name (`tdraw.dll`, `TA PATCH`, `2024.3.31.0`,
`"Swedish Yankspankers & TA Universe community"`).

The reported 680,448-byte community `tdraw.dll` matches **neither** file here. Only TA Patch 3.9.0.2
(`TA_Patch_3902.exe`, 22 MB, 2013) is publicly listed at
`files.tauniverse.com/files/ta/unofficial-patch/`, so no same-version reference build was obtainable and
a byte-for-byte diff was not performed; the export-name evidence settles it regardless.

Other DLLs in 11.3.0: `ddraw.dll` (358,912) and `ddraw_custom.dll` (316,928) are **cnc-ddraw** by
FunkyFr3sh; `tplayx.dll` (298,496) is the TA Patch demo-recorder DirectPlay component.

## Relationship to the community patch stack

Mayhem 11.3.0 does **not** "auto-download the TA Patch" — it **bundles** it and **hard-requires** it.
The shipped `dplayx.dll` (the Patch Loader) refuses to start unless (a) a byte signature at
`game_exe+0x00010000` matches official 3.1, and (b) the exe's DirectDraw import string at `0x004FF618`
has been renamed away from `DDRAW.dll` — otherwise it shows *"Incompatible game files detected…"* or
*"Incompatible tdraw.dll found, please re-install the Total Annihilation Community Patch."* So the load
chain is: `TotalA.exe` → `tdraw.dll` (community patch, replaces `ddraw`) → `dplayx.dll` (Patch Loader,
applies 160 Mayhem hex patches from its embedded INI) → `tplayx.dll` (real DirectPlay) →
`ddraw.dll` (cnc-ddraw) for rendering.

**Does the Patch Loader ship a config for Mayhem? Yes** — `res/mayhem.ini` is one of only three INIs in
the repo (`patches.ini` = plain OTA, `prota.ini` = ProTA, `mayhem.ini` = Total Mayhem), i.e. Mayhem is
one of three first-class supported mods. No `config_*` file exists in either Mayhem package.

## Sources

- [mayhem.tauniverse.com](https://mayhem.tauniverse.com/) — news/changelog, 11.3.0 release notes
- [mayhem.tauniverse.com/totalm.htm](https://mayhem.tauniverse.com/totalm.htm) — version, size, CRC32
- `https://mayhem.tauniverse.com/totalm/TotalM1130.zip` — 11.3.0 package (analysed)
- `https://mayhem.tauniverse.com/totalm/TotalM815.zip` — 8.1.5 package (source of `mdraw.dll`)
- [github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader) — `res/mayhem.ini` (10.9.2), `dllmain.c`, `res/res.rc`
- [files.tauniverse.com/files/ta/unofficial-patch/](https://files.tauniverse.com/files/ta/unofficial-patch/) — public TA Patch archive
- In-package: `1092 to 113 changelog.txt`, `824e and OTA to 109 changelog.txt`, `mayhem.ini`,
  `mayhem.gp3` (`guiM/*.GUI`), stock `TotalA.exe`
