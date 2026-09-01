# TA Zero

## Summary

**TA Zero** is a total conversion for the original Cavedog *Total Annihilation* (`TotalA.exe`, 1997),
authored principally by **Vohvelieläin** — confirmed from a commented-out copyright line inside the
mod's own unit files: `// Copyright=Copyright 2010 Vohvelieläin;` **[VERIFIED]**. Its home is
`zero.tauniverse.com` (per the installer's `ARPURLINFOABOUT` property) with files mirrored on
`files.tauniverse.com/files/ta/mods/ta-zero/`. It descends from an earlier project, **XS_II**
(Alpha 1, 2009 → Alpha 2f, 2010), which sits in the same `old-versions/` directory. **[VERIFIED]**

TA Zero does not ship a bespoke engine. It ships a **hex-edited stock Cavedog `TotalA.exe`** plus
**three renamed proxy DLLs** — the standard TA community-patch architecture:

| File | Role | Renamed from |
|---|---|---|
| `zdraw.dll` | DirectDraw proxy; applies engine patches | `ddraw.dll` / `tdraw.dll` |
| `zplayx.dll` | DirectPlay proxy (TA Demo Recorder lineage) | `dplayx.dll` / `tplayx.dll` |
| `zmusi.dll` | music/`winmm` shim | `tmusi.dll` |

`objdump -p "TA Zero.exe"` shows the import table naming **`ZDRAW.dll`** and **`ZPLAYX.dll`**, while
the binary still carries Cavedog's original build artefacts (`C:\cavedog\wargame\Release\TotalA.pdb`,
`.\Release\TotalA.exe`, `Copyright 0000 Cavedog Entertainment`). That is a five-letter in-place
hex edit of the import-name strings — not a rebuilt executable. **[VERIFIED]**

**Versions.** TAUniverse mirrors Alpha 3 (Jan 2012), 3b, **3c (24 Nov 2012, 22 MB zip)** and
**Alpha 4** (`TA_Zero_Alpha_4.exe`, 04 Oct 2013, 121,067,451 bytes, sha256
`ed86c5c325e43bd9598d5b4fd437f7cfae98ff257c73c27b2dcd7230d021aa02`). **[VERIFIED]** Alpha 5 is
**not** on that mirror; TADR's `tdraw.txt` names `Zero Alpha5-060322` **[VERIFIED]**, which does not
obviously match the "~Dec 2024" date in the brief — I could not reconcile the two.

## The shields question — evidence and verdict

**VERDICT: TA Zero's shields are COB scripts plus stock-engine FBI data tricks. They are NOT an
engine feature. No binary patch is involved.** Confidence: **very high** for Alpha 3c (direct
inspection of the shipped scripts); **high** for Alpha 4/5 by continuity. This is the *same* answer
as TA:ESC — the survey does not, after all, have a case of engine-level shield projectors.

I extracted TA Zero Alpha 3c's data archive `TAZ31.gp3` (a real HPI archive: magic `HAPI`,
version `0x00010000`, header key `0`) with a Python HPI/SQSH decompressor, recovering 153 `.fbi`
unit definitions and 153 `.bos` **script sources** (the mod ships uncompiled BOS alongside COB).

**`GoKT1ShieldNode.fbi` (925 bytes) — the "Shield Node" unit:**

```
Name=Shield Node;
Description=Temporary Shield Wall;
MaxDamage=50;//Armor 1, Void Shield 500
DamageModifier=0.05;
```

There is **no `ShieldRange=` key** — no new FBI key of any kind. `MaxDamage=50` with
`DamageModifier=0.05` is pure stock TA: while the unit is *armored*, the engine multiplies incoming
damage by `DamageModifier`, giving 1000 effective HP; when the script drops the armored flag the
unit reverts to a glass-jawed 50 HP. The author's own comment spells the intent out: `Armor 1,
Void Shield 500`.

**`GoKT1ShieldNode.bos` (3,102 bytes) — the whole shield in script:**

```
static-var  DamageNow, LastDamage, ShieldOn, ShieldBlocks, ShieldPower, Timer;
...
HitByWeapon(AngleX, AngleZ, Impact)
{
    DamageNow = get HEALTH;
    if( ShieldBlocks == 1 )
    {
        turn shieldturn to z-axis (55 * AngleZ) now;
        show shield;
        Impact = 9 * ( LastDamage - DamageNow );
        if( Impact < 5 ) { Impact = 5; }
        ShieldPower = ShieldPower - Impact;
        if( ShieldPower <= 0 ) { set ARMORED to 0; ShieldBlocks = 0; ... }
        return (0);
    }
}
```

Every primitive here shipped with TA in 1997: `HitByWeapon`, `get HEALTH`, `set ARMORED`,
`show`/`hide`, `turn`, `emit-sfx`, `static-var`. The mechanism is:

1. **Damage is measured, not received.** `get HEALTH` returns *percentage* health, so the script
   differences it across the hit (`LastDamage - DamageNow`) and scales by 9 to recover a
   shield-point cost. This is a workaround for the fact that stock COB is never *told* the damage
   number — the decisive tell that no engine hook is feeding it.
2. **A damage floor compensates for that imprecision:** `if( Impact < 5 ) { Impact = 5; }`. This
   matches TA Zero's own changelog verbatim — *"GoK void shield minimum damage per hit increased to
   5 from 4"* and *"Large GoK units and structures with void shield greater than 250 now have higher
   minimum shield damage per hit value, either 7.5 or 10, instead of the standard 5"*. **[VERIFIED]**
3. **Recharge is a script timer loop.** `ShieldUnit()` spins forever incrementing `Timer`, doing
   `ShieldPower = ShieldPower + 2` while `Timer <= 250`, capped at 500. Collapse at `ShieldPower < 0`
   sets `ShieldBlocks = 0` and `set ARMORED to 0`.
4. **Visuals are model pieces.** `gema/gemb/gemc/gemd` are shown/hidden by charge tier
   (500 / 375 / 250 / 0); a `shield` piece is rotated to the impact angle and flashed for 100 ms.

**Mod-wide confirmation** across all 153 FBI + 153 BOS files:

| Probe | Count |
|---|---|
| FBIs containing `ShieldRange` (TADR's engine shield key) | **0** |
| BOS defining a `Shield( ... )` entry point (TADR's engine callback) | **0** |
| BOS declaring a `ShieldPower` static-var | **34** |
| BOS using `set ARMORED` | **68** |
| FBIs setting `DamageModifier` | **68** |

**Binary confirmation.** Raw byte grep for `shield` (case-insensitive) returns **0 hits** in
`TA Zero.exe`, `zdraw.dll`, `zplayx.dll` and `zmusi.dll`. **[VERIFIED]**

**The near-miss worth recording.** TADR's source tree *does* contain a genuine engine-level shield —
in Delphi, in `src/Recorder/plugins/`:

- `UnitInfoExpand.pas` reads a **new FBI key**: `TdfFile_GetInt(..., 'ShieldRange')`.
- `UnitSearchHandlers.pas` `SetShield` assigns `UnitsCustomFields[UnitId].ShieldedBy` to nearby
  allied units and broadcasts the state over the wire.
- `UnitActions.pas` `UnitActions_AntiDamageShield` **hooks the damage path** (registered as
  `MakeRelativeJmp(..., 'dont pass damage to shielded untis', @UnitActions_AntiDamageShieldHook,
  $00499E37, 0)`), suppresses `UNITS_MakeDamage` on the target, and instead calls
  `TAUnit.CobStartScript(p_Shield, 'Shield', @Amount, @UnitId, @TargetUnitId, nil, False)` — handing
  the real damage number to a COB function named `Shield` on the projector.

That is exactly the hybrid design one would want. **But it is not compiled into anything shipped**:
none of `ShieldRange`, `Shield`, `dont pass damage`, `multiairtransport`, `teleportmethod` or
`hidehpbar` appears in any shipped `*playx.dll` (all seven configs) or in any `tdraw.dll`, and the
unit names `UnitActions` / `UnitInfoExpand` / `UnitSearchHandlers` are absent from both TADR's
current `dist/tazero/zplayx.dll` and TA Zero's own 2006-era `zplayx.dll`. **[VERIFIED]**
TA Zero does not use it, and nor does anything else.

## Binary inspection (zdraw.dll vs tdraw.dll)

The brief's key test — "is `zdraw.dll` byte-identical to the 680,448-byte community `tdraw.dll`?" —
turns out to compare two different eras. The answer is nonetheless unambiguous: **`zdraw.dll` is the
community patch**, and TA Zero contributes no engine code of its own.

| File | Bytes | Hash | Note |
|---|---|---|---|
| TA Zero Alpha 3c `zdraw.dll` | 396,800 | sha256 `20e9206f60df5e85…` | 2012-09-23 |
| TA Zero Alpha 3c `zplayx.dll` | 270,336 | sha256 `f51b366c9d2584bf…` | 2006-08-19 |
| TA Zero Alpha 3c `zmusi.dll` | 31,744 | sha256 `6558cf382334b109…` | |
| TA Zero Alpha 3c `TA Zero.exe` | 1,215,488 | sha256 `8a4de16feba8a69e…` | hex-edited Cavedog exe |
| Community `tdraw.dll` (v3.9.02 era) | 680,448 | md5 `e2cd40ae60d7e92b09afa1e97f86c927` | 2022-10-22 |
| TADR `tdraw-tazero.zip` → `tdraw.dll` | 921,088 | md5 `d0cab9731d8770f68166e028861d7ea0`, sha256 `2b9c09e296ddfef1…` | release `v2026.8.6` |
| TADR `dist/tazero/zplayx.dll` | 332,800 | md5 `8c4bf27dd8ea27f07b1023cd357cf5ea` | version `2026.8.29-zero` |

Findings **[VERIFIED]**:

- Alpha 3c's `zdraw.dll` is a **DirectDraw proxy**: its export table's internal name is `ddraw.dll`,
  it exports a single symbol (`DirectDrawCreate`), chain-loads a real `\ddraw.dll`, and reads
  `config\ddraw.ini`. Identical architecture to `tdraw.dll`.
- Alpha 3c's `zplayx.dll` carries the TA Demo Recorder's Delphi units (`PluginEngine`,
  `Dplayx_exports`) — i.e. it *is* a TADR/`tplayx` build, renamed.
- The current TADR `tazero` build of `tdraw.dll` self-identifies at runtime with the string
  **`Process Attached.  config=tazero`**, and its version resource reads
  `CompanyName: TA Demo Recorder & Patches` / `ProductName: TA PATCH`. It is the community patch,
  compiled with a TA-Zero flag — nothing more.

So: **different bytes from the 680,448 build, but not independent engine work.** The size differences
are 2012 vs 2022 vs 2026 releases of the same lineage.

## Other engine claims (pathfinding, sounds)

Both claims are real, both are **community-patch features exposed as configuration**, and neither is
TA Zero's work. Source: `totala.ini` shipped inside `tdraw-tazero.zip`. **[VERIFIED]**

**Pathfinding ×50** — the multiplier is exact:

```
; Pathfinding cycles
; TA v3.1 default is 1333
; TA patch default is 66650
AISearchMapEntries = 66650;
```

66650 / 1333 = **50.0**. Implementation is in TADR's `src/DDraw/LimitCrack.cpp`:
`NowIncreaseAISearchMapEntriesLimit = new IncreaseAISearchMapEntriesLimit(MyConfig->GetIniInt("AISearchMapEntries", 66650))`,
which is a 4-byte overwrite of a hard-coded engine constant via a `SingleHook`. So it *is* an engine
byte patch — applied at runtime by `zdraw.dll`, configurable from an INI, and generic to every mod.
TADR's launcher offers 1333 / 15996 / 33325 / 66650 as presets.

**"Unlimited sounds"** — a registry override written by the patch:

```
; Max number of simultaneous sounds before sounds are cut off
; Set from 2 - 32 for specific limits or 33 or higher for unlimited
; TA v3.1 default is 8 (dword:8)
; TA patch default is 128 / unlimited (dword:128)
"MixingBuffers" = dword:128
```

Alongside `"Sound Mode" = dword:2` (3D positional; stock is mono). TA Zero's Alpha 4 MSI writes
`MixingBuffers` and `Sound Mode` registry values itself, under `Software\TA Patch\…` — i.e. it
configures the community patch rather than implementing anything. **[VERIFIED]**

## Relationship to the community patch stack

**Total dependency, stated by the mod itself.** Alpha 4's embedded readme says, verbatim:
*"Switched to new installer format and now **officially based on the TA Unofficial Patch**"* and
*"-**Based on the TA Patch v3.9.02**"*. Its install instructions are: *"4. Download and install the
TA 3.1c Patch. 5. Download and install the TA Patch Resources. 6. Install Total Annihilation Zero."*
The installer even embeds the prerequisite URL
`http://patch.tauniverse.com/ta-patch-resources/TA_Patch_Resources.exe`. **[VERIFIED]**

**And the dependency is current, not historical.** TADR (the live `tdraw.dll`/`tplayx.dll` project)
carries first-class TA Zero support today:

- `src/DDraw/config_tazero.h` — a full build config (`#define TDRAW_CONFIG_NAME "tazero"`), with
  TA-Zero-specific tuning such as `WEATHER_REPORT_WIND 0` / `WEATHER_REPORT_TIDAL 0` because
  *"TA:Zero's economy has no wind or tidal generators"*.
- `src/Recorder/dist/tazero/zplayx.dll` + a `manifest.csv` row
  `"tazero","zplayx.dll","2026.8.29-zero","332800","8c4bf27dd8ea27f07b1023cd357cf5ea"`.
- A `tdraw-tazero.zip` asset on release `v2026.8.6`.
- `tdraw.txt`'s per-mod table: `Zero Alpha5-060322 | tazero.ini | zdraw.dll`.

Crucially, `tdraw.txt` defines that build as *"**same as tdraw-full.zip except it disables** … wind
and tidal readouts in the weather report … that is all."* The entire TA-Zero-specific engine delta
in the current community patch is **two cosmetic HUD rows**. There is no shield code in it.

## Confidence & gaps

**Shields — very high confidence.** This rests on primary artefacts I extracted and read: the
shipped FBI has no engine shield key, the shipped BOS implements the whole mechanism in stock COB,
the counts hold across all 153 units, and no shield string exists in any TA Zero binary. The
script's `get HEALTH` differencing plus a minimum-damage floor is positive evidence *for* the COB
hypothesis, not merely absence of evidence for the engine one — and TA Zero's own changelog
vocabulary matches the script exactly.

**Gaps, stated plainly:**

- **I could not obtain Alpha 5.** Not on the TAUniverse mirror; `zero.tauniverse.com` and ModDB sit
  behind Cloudflare challenges my tooling could not pass; the TAUniverse forum is login-walled (not
  attempted, per instructions); the session's web-search budget was exhausted before this task
  began. The Alpha 4/5 verdict is an inference from continuity, not direct inspection.
- **Alpha 4's payload is unextracted.** A Caphyon *Advanced Installer* SFX whose file table is
  plaintext (confirming it ships `TotalA.exe`, `zdraw.dll`, `zplayx.dll`, `zmusi.dll`, `TAZ31.gp3`,
  `TAZero.ini`, `SERVER.EXE`) but whose bodies sit in a proprietary LZMA container with no standard
  `LZMA_alone`, CAB, OLE/MSI or ZIP header. Its only extractable 7z stream is `vcredist_x86.exe`.
- **Authorship beyond Vohvelieläin** ("and others") is unconfirmed; the FBI copyright line is the
  only attribution I verified.
- No downloaded binary was executed at any point; all analysis was static.

## Sources

- `https://files.tauniverse.com/files/ta/mods/ta-zero/` and `.../old-versions/` — open directory
  index (not login-walled): `TA_Zero_Alpha_4.exe`, `TA_Zero_Alpha_3c.zip`, XS_II archives.
- TA Zero Alpha 3c contents: `TA Zero.exe`, `zdraw.dll`, `zplayx.dll`, `zmusi.dll`, `TAZ31.gp3`,
  `TAZero.ini`, `Readme (TA Zero).txt`.
- Extracted from `TAZ31.gp3`: `GoKT1ShieldNode.fbi`, `GoKT1ShieldNode.bos`, plus 153 FBI / 153 BOS
  surveyed in aggregate.
- TA Zero Alpha 4 installer — embedded MSI string table and RTF readme/version history.
- `github.com/tanvanman/TADR` — `src/DDraw/config_tazero.h`, `src/DDraw/LimitCrack.cpp`,
  `src/Recorder/tplayx.dpr`, `src/Recorder/dist/manifest.csv`, `src/Recorder/dist/tazero/zplayx.dll`,
  and `src/Recorder/plugins/{UnitActions,UnitInfoExpand,UnitSearchHandlers}.pas`.
- TADR release `v2026.8.6` asset `tdraw-tazero.zip` → `tdraw.dll`, `tdraw.txt`, `totala.ini`.
