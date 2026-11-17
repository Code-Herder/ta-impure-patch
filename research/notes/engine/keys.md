# Data keys the engine reads

The TDF getters, the parsers that call them, and every key each parser reads: the unit FBI, the move classes, weapons, features, the map's OTA and the sidedata. The inventory tasim's coverage is measured against.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

**How this page was made** [DISASSEMBLED 2026-11-11]: every call to the getters below was listed
from `objdump -d -M intel` of `pristine/TotalA.exe.pristine`, and for each call the key string it
pushes, the default and the store that follows were read. A key is listed with its destination
only where that store was read; "not traced" marks the rest. *Who reads each field*, below, lists
the code that reads the main UnitDef and weapon fields back (a sweep of every instruction with
the field's displacement, each base register traced to a def or weapon pointer); what is still open
is *Not yet mapped*, at the end.

## The getters

A TDF section object holds a sorted vector of `{key*, value*}` pairs from `+0x19` (begin) to
`+0x1D` (end); every getter bisects it with `_stricmp 0x4F8A70` (a lower bound, signed halving), so
**keys are case-insensitive**. **The default is used only when the key is absent** (or its value
pointer is NULL): a present key whose text is not a number gives **0** through all three number
getters, never the default [DISASSEMBLED, scout-Jb J7, `0x4C46C0..0x4C4878`].

| Getter | Convention | Returns | Callers |
|---|---|---|---|
| `0x4C46C0` | `thiscall(section; key, int default)`, `ret 8` | `atoi` (`0x4E4F70`, a jump to `atol 0x4E4ED0`) of the value, else the default. `atol` skips `isspace` (the ctype table's bit `0x08`, `0x4E4ED8..0x4E4F07`), takes one sign and decimal digits, `n·10 + d` in 32 bits with **no overflow test**, so it wraps: `"1.6"` → 1, `"-0.5"` → 0, `"4294967297"` → 1 | 287 |
| `0x4C4760` | `thiscall(section; key, double default)`, `ret 0xC` | `atof` (`0x4E4560`: `isspace` skipped, `_fltin 0x4EAF00`) on the x87 stack, else the default; **0.0 when the text has no digits** (`_fltin`'s flag `0x4`, `0x4EAF25..0x4EAF3F`). The double is the correctly rounded one [INFERRED: `0x4F3D10`/`0x4F3990` not traced. MEASURED 2026-11-12, Wine's MSVCRT `atof` equals a correctly rounded parser on all 51 631 values of the retail install's unit, weapon, feature and move-class files; Wine's CRT is not the game's static one, so the engine's own stores are `tools/tasim/defcheck.py`'s to compare] | 36 |
| `0x4C48C0` | `thiscall(section; out, key, size, default*)`, `ret 0x10` | copies at most size − 1 characters (`0x4E4760`) and returns 1, else copies the default ([memory](memory.html), *the TDF string reader*) | 120 |
| `0x4C4800` | `thiscall(section; int* out, key, int default)`, `ret 0xC` | `atof` × 65536.0 (`0x4FDC20`, exact) truncated toward zero by `_ftol 0x4E43A0` (chop, `fistp` QWORD, the low dword: 32768.0 and above wrap, past the int64 range 0) into `*out`: 16.16 fixed point; returns `out`. The `fmul` is at `0x4C4884` and the store `*out` at `0x4C4896` (`0x4C4800..0x4C4898`). **The default is stored as pushed, unscaled** (`0x4C48A2..0x4C48AD`): every caller pushes it in 16.16 already. `"1.6"` → 104857, `"-1.6"` → −104857. MEASURED 2026-11-12: this `fmul`/`_ftol` sequence run on the x87 over Wine's MSVCRT `atof` equals tasim's `ts_tdf_fx` on all 51 631 retail values (`tagpu/ddraw/tests/tasim_keys_test.py`) | 8, all in the FBI parser |
| `0x4C4630` | `thiscall(section; key)`, `ret 4` | the value's string pointer, or NULL | 6 |
| `0x4C45E0` | `thiscall(section; n)`, `ret 4` | the n-th pair: walks a section whose keys are data | 2 (a weapon's damage section) |
| `0x4C58A0` | `ret 0x14`; `(section, out, key, size, default)` [INFERRED from its call sites] | the key with the language prefix `0x51FDC0` [INFERRED: a localized lookup; its fallback order not traced] | 8 |

Caller counts are `call rel32` instructions in `.text`.

**tasim's key table** (`tagpu/ddraw/src/tasim_keys.c`) holds every key of this page, in each parser's
order, with its getter, its default and the store after it: the one rule by which tasim turns a key into
the field the engine holds. `tools/tasim/defcheck.py` compares what it loads with a running game's tables,
byte for byte.

## The parsers

| File | Parser | Callers |
|---|---|---|
| a unit's FBI `[UNITINFO]` → its UnitDef (stride `0x249`, `ebp` = the def) | `0x42BF40` | `0x42D269`, `0x42D722` (the unit-data load `0x42D2E0`, loader thread) |
| a move class → a `0x20`-byte record: moveinfo's set once a process by `0x440230`, a unit's own by `0x4402E0` | `0x440340` | `0x42D39B` (moveinfo's classes, 32 records at `0x512358..0x512758`: the loop `0x42D328..0x42D3AA` takes `CLASS%d` for i = 0..31, reads its `name` through `0x4C48C0` into a 100-byte buffer, keeps a `0x4D8610` copy's pointer at the record's `+0` (`0x42D365..0x42D392`), and parses the section over the record's current values), `0x42CD51` (a unit with no `movementclass`: its own FBI section) |
| a weapon TDF → the weapon record (stock layout `main+0x2CF3 + ID·0x115`; the raised limits move it, [weapons](weapons.html)) | `0x42E440` | `0x42E3E7` in the weapon load `0x42E310` |
| a feature TDF → its FeatureDef (256 bytes, `main+0x14253`-indexed) | `0x4224B0` | `0x422026`, `0x422E90`, `0x422FA4`, `0x42304C`, `0x4230F4`, `0x4231DE`, `0x424DE1` |
| a feature's successors | `0x422EA0` | `0x424ECD`, `0x4918D9` |
| the menu-time unit pre-scan (the CRC and incompatible-units check) | `0x42A8D0` | `0x42BD29` |
| the unit-data load, with the sidedata `CANBUILD` lists | `0x42D2E0` | `0x4918CA` |
| the download TDFs ([the GUI](ui.html), *The download menus' records*) | `0x42DCF0` | `0x4918CF` |
| the map's OTA | `0x435DA0` | `0x43528F`, `0x435A5A`, `0x435BDF`, `0x435C16`, `0x435D02` |
| mission and campaign scripts | `0x436C30`, `0x48E010`, `0x436860` | keys not listed here |
| `.GUI` files | `0x4AD350`, `0x4AD890`, `0x4ADC70`, `0x4AEAC0` | [gui-gadgets](../gui-gadgets.html) |

The load functions' own entries: the FBI loader and the unit-data load in [memory](memory.html), the weapon parser in [weapons](weapons.html) (*A weapon's ID*), the download TDFs in [the GUI](ui.html).

## Unit FBI keys

Read by the FBI loader `0x42BF40`. Integers and floats default to 0 and strings to "" (`0x5119B8`) unless a default is given.
`fx` = 16.16 through `0x4C4800`; `dbl` = through `0x4C4760`; `int` = through `0x4C46C0`.

- **Strings.** `unitname` (def `+0x20`, 32 bytes); `name` and `description` (both through
  `0x4C58A0`; `name` at def `+0x00`, `description` at `+0x40`); `objectname` (`+0x80`, 32 bytes,
  default the `unitname`, `0x42C0B1..0x42C0EA`); `defaultmissiontype` (`+0x230`, a byte: the action's
  index through `0x438760`, `0x42BFD7..0x42C018`); `wpri_badTargetCategory`, `wsec_badTargetCategory`,
  `wspe_badTargetCategory` and `noChaseCategory` (default `none`, each stored as the category's
  mask pointer from `0x488C50`: `+0x231`, `+0x235`, `+0x239`, `+0x23D`); `category` (no def field:
  `0x488E70(def, string)` splits it on white space and sets the def's row bit, `+0x21E`, in each
  named category's mask, and always in `ALL`, `0x42CC1F..0x42CC40`); `soundcategory` (`+0x20E`, the
  index of the matching sound category, else `atoi` of the value, 0 when absent,
  `0x42CC45..0x42CCB8`); `corpse` (`+0x1BC`, a feature index through `0x422E40`, `0xFFFF` when
  absent); `movementclass` (→ `0x440420` → the class pointer `+0x1B6`: one of the 32 records `0x512358 + 0x20·c`, or 0 when no class has the name, written `0x42CD29`; 0 with no key, `0x42CD0C`; `0x440420..0x440463`); `weapon1`..`weapon3`
  (→ `+0x1EE`, `+0x1F2`, `+0x1F6` through `0x49E5B0`, else the null record `main+0x2CF3`;
  [extra weapons](../extra-weapons.html) adds more), `explodeas` (→ `+0x220`) and `selfdestructas`
  (→ `+0x224`), weapon pointers by the same lookup, each name read into a 0x80-byte buffer (`0x42CDD2`); `YardMap` (→ `+0x14E`, a FootprintX × FootprintZ
  byte map, read into a 0x400-byte buffer (`0x42CF47`), **only when `bmcode` is 0**, else `+0x14E` = 0, `0x42CF2A..0x42D073`; [units](units.html));
  `selfdestructcountdown` read raw through `0x4C4630` into def `+0x245` bits 20–22 (`atoi & 7`),
  **5 when absent** (`0x42CBD9..0x42CC19`).
- **Costs and economy.** `buildcostenergy` and `buildcostmetal` (int, stored as floats `+0x186`,
  `+0x18A`); dbl, stored as floats: `energymake` `+0x1C2`, `energyuse` `+0x1C6`, `metalmake`
  `+0x1CA`, `extractsmetal` `+0x1CE`, `windgenerator` `+0x1D2`, `tidalgenerator` `+0x1D6`,
  `energystorage` `+0x1E2`, `metalstorage` `+0x1E6`; `makesmetal` int, byte `+0x22D`.
- **Motion.** fx: `maxvelocity` `+0x192`, `brakerate` `+0x19A`, `acceleration` `+0x19E`,
  `bankscale` `+0x1A2` (default 1.0), `pitchscale` `+0x1A6`, `damagemodifier` `+0x1AA` (default 1.0),
  `moverate1` `+0x1AE` and `moverate2` `+0x1B2` (default twice `maxvelocity`: its dword `shl 1`, so in 32
  bits, `0x42C1E6`). Derived: `+0x196` = `maxvelocity` ÷ (`maxslope` + 1), the truncated quotient of
  `maxvelocity << 16` over `(maxslope + 1) << 16` through `_allshl`/`_alldiv` (`0x42CD9D..0x42CDC7`), with
  the `maxslope` the class record gave [its meaning INFERRED]. The footprint box, 16.16, each half
  truncated toward zero (`cdq; sub; sar 1`), `0x42D079..0x42D11F`: `+0x15E` = −(FootprintX << 20) / 2,
  `+0x166` = −(FootprintZ << 20) / 2, `+0x16A` = (FootprintX << 20) / 2, `+0x172` = (FootprintZ << 20) / 2,
  `+0x176` = `+0x16A` − `+0x15E`, `+0x17A` = `+0x16E` − `+0x162`, `+0x17E` = `+0x172` − `+0x166`, `+0x182` =
  (`+0x176` + `+0x17E`) / 3 (the `0x55555556` multiply, truncated); `+0x162` and `+0x16E` are not written
  by the parser. int: `turnrate` (word `+0x1BA`), `waterline` (byte `+0x22C`),
  `transportsize` (byte `+0x22A`), `transportcapacity` (byte `+0x22B`), `cruisealt` (word `+0x21C`),
  `maneuverleashlength` (word `+0x214`), `attackrunlength` (word `+0x216`), `kamikazedistance`
  (word `+0x218`).
- **Build and health.** `buildtime` (dword `+0x1EA`), `workertime` (word `+0x1FE`), `healtime`
  (word `+0x200`), `maxdamage` (dword `+0x1FA`), `builddistance` (word `+0x212`), `buildangle`
  (word `+0x210`), `bmcode` (byte `+0x22F`, unmasked: 0 a building, 1 a mover, any other value neither;
  [units](units.html)).
- **Sensors.** words: `sightdistance` `+0x202`, `radardistance` `+0x204`, `sonardistance` `+0x206`,
  `mincloakdistance` `+0x208`, `radardistancejam` `+0x20A`, `sonardistancejam` `+0x20C`. `cloakcost`
  (int, stored as a float, `+0x1DA`) and `cloakcostmoving` (`+0x1DE`, default the `cloakcost`'s int: `fst`
  leaves the `fild`'ed int on the stack and `_ftol` pushes it, `0x42C512..0x42C525`, so a cost above 2²⁴ is
  rounded once, as stored, not twice);
  `mincloakdistance` is set to 80 when the unit can cloak (bit 13 below) and the key left it 0
  (`0x42D12A..0x42D13F`).
- **Other ints.** `sortbias` (word `+0x21A`); `standingmoveorder` (default **2**, def `+0x241` bits
  0–1, the int `& 3`) and `standingfireorder` (default **2**, bits 2–3).
- **Flags in def `+0x241`**, one key a bit (each read masked to bit 0 and shifted in):
  `init_cloaked` 4, `downloadable` 5, `builder` 6, `zbuffer` 7, `stealth` 8, `isairbase` 9,
  `istargetingupgrade` 10, `canfly` 11, `canhover` 12, `teleporter` 13, `hidedamage` 14, `shootme`
  15, `armoredstate` 17, `activatewhenbuilt` 18, `floater` 19, `upright` 20, `amphibious` 21,
  `isfeature` 24, `noshadow` 25, `immunetoparalyzer` 26, `hoverattack` 27, `kamikaze` 28,
  `antiweapons` 29, `digger` 30. **Not keys**: bit 16 is set when any of `weapon1`..`weapon3` is
  not the null record (`0x42CEF2..0x42CF24`); bit 23 is the type's availability, which the pre-scan's
  tail (`0x42B248..0x42B2E8`) and the unit-data load `0x42D2E0` (`0x42D493..0x42D510`) drop every type
  without: the pre-scan writes it from the file's `Version`, where it came from and its `Copyright` (*Read
  only by the pre-scan* below), and `0x431740` and `0x46E160` write it later; bit 31 is
  the orders-page bit ([orders](orders.html)). Bit 22 has no writer in `.text` besides the def copy
  `0x42B370`, but `0x44E4A7` and `0x490727` test it [INFERRED: always clear in stock].
- **Flags in def `+0x245`**: `mobilestandorders` 0, `firestandorders` 1, `onoffable` 2, `canstop` 3,
  `canattack` 4, `canguard` 5, `canpatrol` 6, `canmove` 7, `canload` 8, `canreclamate` 10,
  `canresurrect` 11, `cancapture` 12, `candgun` 14, `norestrict` 15, `showplayername` 17,
  `commander` 18, `cantbetransported` 19, `selfdestructcountdown` 20–22. Bit 16 is `wacky`, written
  only by the pre-scan (`0x42ADA8..0x42ADCA`). **Bit 9 has no key**: the loader copies bit 10 into it
  (`0x42CA3B..0x42CA4F`); the resolver reads it as the repair capability
  ([orders](orders.html), *What each resolver mode answers*). **Bit 13 has no key**: it is set when
  `cloakcost` (`+0x1DA`) is above 0 (`0x42CA5A..0x42CA99`) [INFERRED: CanCloak].
- **Movement**, through `0x440340` from the named class or the unit's own section: `FootPrintX`
  (word, record `+4`), `FootPrintZ` (`+6`), `maxwaterdepth` (`+8`, default 10000), `minwaterdepth`
  (`+0xA`, default −10000), `maxslope` (byte `+0xC`, default 255), `badslope` (`+0xD`, default half of
  `maxslope`), `maxwaterslope` (`+0xE`, default 255), `badwaterslope` (`+0xF`, default half of
  `maxwaterslope`). The footprint's default is 0 (`push 0`, `0x440349`, `0x44035F`); every other default
  is the record's own value as it stands when the parse begins (`0x440372`, `0x440381`, `0x44039D`,
  `0x4403CE`), except `badslope`'s and `badwaterslope`'s, which are the low byte of the value just read
  for `maxslope` or `maxwaterslope`, shifted right once (`0x4403AE..0x4403BB`, before any clamp). A unit's
  own section is parsed into a stack record `0x4402E0` has just set (footprint 0, the depths 10000 and
  −10000, all four slopes 255). **moveinfo's records keep their bytes from one level to the next**: the
  static constructor `0x440230` sets the 32 records to those same values once a process
  (`0x440233..0x440279`); the level teardown `0x440A00` clears only `+0` (the name), `+0x10`, `+0x14` and
  `+0x18` (`0x440A09..0x440A26`); and every level load parses each `CLASS<i>` over its record's current
  bytes (`0x42D39B`) [DISASSEMBLED 2026-11-12]. From a process's second level on, an absent key therefore
  takes the value the last load left, which is the first load's for every key but one: a class that
  omits `maxslope` and `badslope` and sets `maxwaterslope` below 255 gets `badslope` = `maxwaterslope >> 1`,
  its `maxslope` default being the last load's clamped `maxslope` (`maxwaterslope`), where a process's
  first load gives min(127, `maxwaterslope`). The `moveinfo.tdf` of the retail install, ProTA 4.8 and
  Total Mayhem 11.3 has no such class (MEASURED 2026-11-12, every `CLASS<i>` section). It then clamps, unsigned, `maxslope` ≤ `maxwaterslope`, `badslope` ≤ `maxslope` and
  `badwaterslope` ≤ `maxwaterslope` (`0x4403F4..0x440418`). **A named class replaces the unit's own
  keys**: when `movementclass` names one of the 32 records (`0x440420`, `_stricmp` over the records that
  have a name), the loader copies that record, and the unit's own `FootPrintX` … `badwaterslope` are
  never read; only with no key, or a name no record has, does it read the unit's own section through
  `0x440340` into a stack record initialised by `0x4402E0` (called at `0x42CD36`; `0x42CD2F..0x42CD51`) [DISASSEMBLED 2026-11-12]. The unit gets `+4`, `+6`, `+8`, `+0xA`, `+0xC`, `+0xE` at def `+0x14A`,
  `+0x14C`, `+0x1BE`, `+0x1C0`, `+0x228`, `+0x229` (`0x42CD5D..0x42CD97`); `badslope` and
  `badwaterslope` stay in the class record, which `+0x1B6` points at when a class is named.
- **Read only by the pre-scan `0x42A8D0`**: `side`, `ai_weight`, `ai_limit`, `wacky`, `Version`,
  `Copyright`; it also reads `unitname`, `name`, `objectname`, the two costs, `norestrict`,
  `weapon1`..`weapon3`, `explodeas` and `selfdestructas`. Three of them decide bit 23, in this order
  [DISASSEMBLED 2026-11-12]. **`Version`** (`0x4C4760`, default 0.0) is V; major = `_ftol(floor(V))`,
  minor = `_ftol(floor((V − major) · 10.0))`, the floors the CRT's `0x4E5050` and the product stored as a
  double (`0x42B01F..0x42B05E`), so `3.3` has minor 2; the bit is set when major < `main+1`, or major =
  `main+1` and minor ≤ `main+2`, and cleared otherwise (`0x42B066..0x42B0A7`, signed bytes, `movsx` at
  `0x42B06C` and `0x42B083`; the exe
  writes 3 and 1 there at `0x49E9BD` and `0x49E9C6`, a Patch Loader's `MultiplayerVersionMajor`/`Minor`
  other values). **The file's source**: cleared when `0x4BB650(handle)` says it did not come from an
  archive and `0x50289C` is set (1 in the exe, written by no instruction), and whenever `0x511DE4` is set
  (`0x42B0AD..0x42B0DC`). **`Copyright`** (`0x4C48C0` at `0x42B0FD` into `0x80` bytes, default `Run to the Village!
  Warn your brother!` `0x5037BC`, pushed at `0x42B0E6`), with its bytes 10..13 overwritten by `0000` (the
  offset `strstr` finds in the reference, `0x42AB0C..0x42AB20`; the template `0x50390C` and the reference
  `0x5038CC` pushed at `0x42AAB7` and `0x42AABF`, re-read at `0x42B102` and `0x42B107`), must equal `Copyright 0000 Humongous Entertainment. All rights
  reserved.` byte for byte, or the bit is cleared (`0x42B0E2..0x42B162`).
- **Sidedata**: `canbuild1`..`canbuildN` under `[CANBUILD]` (the format `canbuild%d`, `0x503ED8`,
  at `0x42DA14`).

## Weapon TDF keys

Read by the weapon parser `0x42E440`. The record's fields and `+0x111`'s bits, with how each stored value is converted, are
[weapons](weapons.html), *What the loader writes into the record*; what the parser reads:

- `ID` (int, default −1; selects the record, unbounded in stock: Impure's loader skips an ID outside the
  array, [weapons](weapons.html)), `name` (`+0x20`, 0x40 bytes). The record's `+0` is the **section's
  name**, copied first, and it is what a unit's `weapon1`… names: `0x49E5B0(name)` answers the lowest
  slot whose `+0` equals the name under `_stricmp`, and NULL for an empty name or no match, which the FBI
  loader replaces with the null record `main+0x2CF3` (`0x49E5B0..0x49E603`, DISASSEMBLED 2026-11-12).
- The constants, MEASURED 2026-11-12 (the doubles' bytes): `0x4FD240` and `0x4FD248` are the doubles
  nearest 65536/30 and 65536/900, `0x4FD250` is 30.0 and `0x4FD258` the double nearest 1/30; **`0x4FD260`
  is `0x1.1df46a2529d34p-6`, 5 ulp below the double nearest π/180**.
- dbl: `weaponvelocity` (`+0x68`) and `startvelocity` (`+0x6C`), both × 65536/30 (`0x4FD240`),
  `weaponacceleration` (`+0x70`; × 65536/900, `0x4FD248`), `energypershot` (float `+0xC0`),
  `metalpershot` (float `+0xC4`), `edgeeffectiveness` (float `+0xD8`), `minbarrelangle` (float
  `+0xC8`, × π/180); seconds to ticks, × 30 (`0x4FD250`) into words: `reloadtime` `+0xE4`,
  `weapontimer` `+0xE6`, `burstrate` `+0xEC`, `duration` `+0xF0`, `randomdecay` `+0xF2`, `smokedelay`
  `+0xFA`, `flighttime` `+0xFC`, `holdtime` `+0xFE`, and `shakeduration` (dword `+0xD0`); `turnrate`
  × 1/30 (`0x4FD258`) into the word `+0xE8`.
- int: `range` (dword `+0xDC`, default `0x7FFF`), `coverage` (`+0xE0`), `areaofeffect` (word
  `+0xD6`), `burst` (`+0xEA`), `sprayangle` (`+0xEE`), `firestarter` (byte `+0x10B`), `rendertype`
  (`+0x10C`), `color` (`+0x10D`), `color2` (`+0x10E`), `accuracy` (word `+0x104`), `tolerance`
  (`+0x106`), `pitchtolerance` (`+0x108`), `shakemagnitude` (`+0xCC`), `default` (the damage, word
  `+0xD4`, 0 when the weapon has no damage sub-section; `0x42EF7F..0x42EFA9`), and that sub-section walked by index
  (`0x4C45E0`; its keys are unit names) into a table at `+0x64`.
- Flags in `+0x111`: `lineofsight` 0, `ballistic` 1, `shellweapon` 2, `beamweapon` 3, `vlaunch` 4,
  `meteor` 5, `noradar` 6, `paralyzer` 7, `dropped` 8, `startsmoke` 9, `endsmoke` 10,
  `soundtrigger` 11, `guidance` 12, `tracks` 13, `unitsonly` 14, `groundbounce` 15, `waterweapon`
  16, `toairweapon` 17, `smoketrail` 18, `turret` 19, `selfprop` 20, `propeller` 21, `noexplode` 22,
  `burnblow` 23, `twophase` 24, `cruise` 25, `commandfire` 26, `noautorange` 27, `stockpile` 28,
  `targetable` 29, `interceptor` 30.
- Strings: `model`, `explosiongaf`, `explosionart`, `lavaexplosiongaf`, `lavaexplosionart`,
  `waterexplosiongaf`, `waterexplosionart`; `soundstart`, `soundhit`, `soundwater` (sound indices,
  words `+0xF4`, `+0xF6`, `+0xF8`, `0xFFFF` when absent).
- Not keys: `+0x10A` is the weapon's ID byte, written by the weapon load `0x42E310` for every record
  (`0x42E322`); the parser's last call `0x49E010(record)` sets the fire handler `+0x60` from the
  flags: `turret` → `0x49D580`, else `vlaunch` → `0x49DB70`, else `lineofsight` or `selfprop` →
  `0x49D9C0`, else `dropped` → `0x49DD60` (`0x49E014..0x49E065`).

## Feature keys — `0x4224B0`

Defaults 0 and "". `Description`, `footprintx` (word `+0x94`), `footprintz` (`+0x96`), `height`
(byte `+0xFA`), `object`, `filename`, `seqname`, `seqnameshad`, `seqnameburn`, `seqnameburnshad`,
`seqnamedie`, `seqnamedieshad`, `seqnamereclamate`, `seqnamereclamateshad`, `spreadchance` (byte
`+0xFB`), `reproduce` (`+0xFC`), `reproducearea` (`+0xFD`), `metal` (int, **its low 16 bits** as a float
`+0xF0`: `and eax,0xFFFF` before the `fild`, `0x422A45`, so 70000 is 4464), `energy` (the same, `+0xEC`),
`damage` (word `+0xEA`), `sparktime` (dbl × 30.0 at `0x4FD038`, chopped, word `+0xE8`), `burnweapon`
(`+0xE4`, a weapon record through `0x49E5B0`). The name is the section's, `+0x00` (0x80 bytes);
`Description` is `+0x80` (0x14 bytes). **With an `object` key the GAF keys are not read**: the 3DO path
clears `+0xFE` bit 0 and jumps past `filename` … `seqnamereclamateshad` (`0x42261B`); without one bit 0
is set and they are read [DISASSEMBLED 2026-11-12]. Flags in the word `+0xFE`: `animating` 1, `animtrans` 2, `shadtrans` 3, `flamable` 4,
`geothermal` 5, `blocking` 6, `reclaimable` 7, `autoreclaimable` 8 (default 1), `indestructible` 9,
`nodisplayinfo` 10, `nodrawundergray` 11; bit 11 is set whatever the key says for the four names
`DragonsTeeth`, `DragonsTeeth_Core`, `Fortification` and `Fortification_Core` (`0x422C50..0x422CDA`). The successors, `0x422EA0`: `featuredead`,
`featurereclamate`, `featureburnt`.

## Map OTA keys — `0x435DA0`

`missionfile`, `missionname`, `brief`, `narration`, `missionhint` (the last four through
`0x4C58A0`), `maxunits`, `glamour`, `glamoursound`, `UseOnlyUnits`, `mapping`, `lineofsight`,
`memory`, `numplayers`, `Planet`, `nomovie`, `missiondescription`, `minwindspeed`, `maxwindspeed`,
`gravity`, `tidalstrength`, `lavaworld`, `nosealeveltrigger`, `waterdoesdamage`, `waterdamage`,
`killmul`, `timemul`, `HumanMetal`, `HumanEnergy`, `ComputerMetal`, `ComputerEnergy`,
`SurfaceMetal` (the map struct's `+0xD30`, [map](map.html)), `aiprofile` (`+0x804`,
[orders](orders.html) *The AI object*), `MeteorWeapon`, `MeteorRadius`, `MeteorDensity`,
`MeteorDuration`, `MeteorInterval`, and in the start-position sections `type` and `specialwhat`.

## Who reads each field [DISASSEMBLED 2026-11-11]

Every instruction in `.text` with the field's displacement, `esp`-based ones excluded, each base
register traced back to a def (`[unit+0x92]`, or an index into `main+0x1439B`) or a weapon record
(`[def+0x1EE..]`, a unit's weapon slot, `[proj+0]`, `0x49E5B0`'s answer); writers (the parser, the
pre-scan and the def copy `0x42B370`) are not listed. **Limits**: objdump's linear sweep loses a few
bytes after jump tables embedded in `.text`; a field copied into the unit and read there (the unit's
`+0x110` bits made from the def flags by `0x485A40`) is not a def read; Impure's own readers are not
covered.

**UnitDef**

| Field | Read by |
|---|---|
| `+0x186` / `+0x18A` costs | the build step `0x41BA60` (on the builder's ledger), the unbuild `0x41BCD0`, the repair step `0x41BD10`, BuildingBuild `0x402640` (metal × (1 − `+0x104`)), Capture `0x404270`, the unit reclaim step `0x438650`, the destructor's kill credit `0x486CC9`, the stock AI's rating `0x409730`; UI: the bottom bar's build-button hover `0x46AE5E`, the RESTRICT screen, the UNITINFO window's `0x489280` |
| `+0x1C2` `energymake`, `+0x1CA` `metalmake`, `+0x1E2`/`+0x1E6` storages | the economy pass `0x401360` only (and `energymake` the stock AI's rating) |
| `+0x1C6` `energyuse`, `+0x1D2` `windgenerator`, `+0x1D6` `tidalgenerator` | the economy pass, and `0x488F30(def)` (`ret 4`, a float): `energyuse` when non-zero, else −`windgenerator` × the wind `main+0x37EDE`, else −`tidalgenerator` × the tide `main+0x14267`, else 0 (`0x488F30..0x488FA9`) — it **ignores `energymake`**; called only by the stock AI's rating and two uncalled functions. The wind turbine's `0x437910` reads `windgenerator` |
| `+0x1CE` `extractsmetal` | the economy pass, the extractor's `SetSpeed` `0x437840`, the stock AI's rating and its build-site picker `0x40BFE0` |
| `+0x1EA` `buildtime` | the build step (progress = work ÷ buildtime, `0x41BACD`), the unbuild, the repair step, Resurrect `0x404FB0`, `0x489280` |
| `+0x1FA` `maxdamage` | every "damaged?" and HP-percentage test: the repair orders, Guard, the VTOL attack and patrol states, the build and repair steps, the unit reclaim step, the resolver (`0x43F935`), COB's `HEALTH` (`0x4807E2`), the type init `0x485B17`, death `0x486578`, damage `0x489CE0`, the unit tick's HP bytes (`0x48AE8D`, `0x48AF50`), the turret fire handler's accuracy (`0x49D6CE`), AutoAim (`0x49E4A5`), the bottom bar |
| `+0x1FE` `workertime` | every build, assist, repair, resurrect, capture and reclaim order, divided by 30 for a per-tick rate |
| `+0x202` `sightdistance` | the sight emitters (`0x4827B0`, `0x482AC0`, `0x482090`, `0x4816A0`), the destructor (`0x48673E`), the range circles `0x4390A0`; and order logic: RepairPatrol's and VTOL_RepairPatrol's search radius, VTOL_SeekGuard, the stock AI's `0x40B7B0`, `0x43B400` |
| `+0x204` `radardistance` | only the viewed player's map build `0x467440`, the minimap dots `0x466DC0`, the range circles and the stock AI's rating: **nothing in the unit tick, the orders, the resolvers or the fire and target code** |
| `+0x212` `builddistance` | MobileBuild, HelpBuild (plus half of 16 · √(fx² + 2·fz) of the target's footprint, which squares fx alone, so a target deeper than two cells adds less than half its diagonal; [orders](orders.html), *HelpBuild's walk and its abandon*), Capture, ReclaimUnit, RepairUnit and their VTOL forms; the range circles. A feature's Reclaim `0x404AD0` does not read it |

**Flag bits with no reader from a def pointer**: `+0x241` bits 13 `teleporter` and 17 `armoredstate`,
and `+0x245` bit 3 `canstop`. `+0x241` bit 27 `hoverattack` has two readers, both in the action resolver's
attack case: `test ecx,0x8000000` at `0x43F2FA` and at `0x43F323`, on the def word `+0x241` loaded at
`0x43F2F0` (DISASSEMBLED 2026-11-12, scout N-b3). `+0x241` bit 10 `istargetingupgrade` is read only by
the stock AI's scan `0x40AA40`; `+0x245` bit 18 `commander` only by the bottom bar (the player's
name under a commander). The type init `0x485A40` copies `+0x241` bits 0–4, 7, 9 and 16 into the
unit (bit 16 → unit `+0x110` bit 31, bits 0–1 and 2–3 → the standing states, [orders](orders.html)).

**Weapon record**

| Field | Read by |
|---|---|
| `+0xC0` / `+0xC4` per-shot cost | AutoAim `0x49E1A0`: a weapon that is not a stockpile fires only while the owner holds the energy and metal (`0x49E3F3..0x49E40C`) and pays them through `0x4012A0`; the stockpile build `BuildWeapon 0x402B70`, prorated over `reloadtime`; the cursor resolver `0x43E490` for a D-gun |
| `+0xD4` damage | `0x499CD0(proj, target)`: `+0xD4` unless the `+0x64` table names the target's type; the feature hit `0x4244B0`; the stock AI's ratings |
| `+0xD8` `edgeeffectiveness` | area damage `0x49A120` only (`0x49A3AA`) |
| `+0xDC` `range` | the range test `0x49AA80`, can-engage `0x49ABB0`, `0x49ADF0`, the fire paths after their `noautorange` tests; the aircraft states take weapon 1's range as their engagement distance; the range circles; the stock AI's ratings |
| `+0x104` `accuracy` | **only the turret fire handler `0x49D580`** (`0x49D6D7`): the spread is (accuracy − h + 0x800) ÷ k, h from the unit's HP ratio and k from `unit+0xB8` ÷ 12 [INFERRED: veterancy], and a non-zero spread draws the simulation RNG twice (`0x49D727`, `0x49D733`) |
| `+0x106` / `+0x108` tolerances | only the aim test `0x49D880`: a 0 `tolerance` means 2000 while the shooter moves and 150 otherwise (`0x49D889..0x49D8B2`); a 0 `pitchtolerance` means the tolerance |

Of `+0x111`'s bits, 2 `shellweapon` has no reader, 12 `guidance` is read only by the projectile tick
(`0x49BA4C..0x49BA51`, for a selfprop shot that is not twophase) and 13 `tracks` by the tick twice, the second
at the twophase phase change (`0x49BB2F`), and 6 `noradar` is read only by the minimap dots ([weapons](weapons.html)
has each bit's readers and *What guides a shot*).

## Keys Impure adds

Read by the DLL, not the exe ([data keys](../modules/data-keys.html)):

- Unit FBI: `weapon4`..`weapon10` and `w4_badTargetCategory`..`w10_badTargetCategory`
  (`tagpu_weapons.c`); `VeterancyThresholds`, `VeterancyAccuracyBuffRate`,
  `TransportedExplodeAs`, `TransportedSelfDestructAs` and `PreviewPieces` (`tagpu_datakeys.c`).
- Weapon TDF: `nottoair`, `nottounderwater`, `surfacefire`, `notoverwater`, `notoverland`,
  `nomapweaponalert` (`tagpu_datakeys.c`).

## Not yet mapped

- **Readers of the remaining fields**: `maxvelocity` `+0x192`, `+0x196`, `bankscale`,
  `transportcapacity`, the cloak costs beyond the economy pass, the weapon's velocities, `turnrate`,
  `burst`, `sprayangle` and `areaofeffect`; and every read of a def flag *after* it is copied into
  the unit.
- What `0x43B400`, `0x40B530`, `0x43A360`, `0x43A420`, `0x46D040`, `0x46D2E0`, `0x467840`,
  `0x48A490` and `0x48A7F0` do beyond the field or bit they read; def `+0x241` bit 23's
  exact meaning and its writers' callers.
- The `.GUI` parsers' and the mission parsers' keys in full (listed by parser only).
- `0x4C58A0`'s fallback order when the language-prefixed key is missing.
- `ai_limit` (`+0xFE`): no instruction reads it through a `lea` besides the pre-scan and the def copy,
  so an FBI's `ai_limit` has no effect [INFERRED: a stock bug; a plain `mov` from a def's `+0xFE` was
  not separated from the other `+0xFE` operands]. `ai_weight` (`+0xBE`) is run as profile text by
  `0x409F80` and `0x40A040` ([orders](orders.html), *The stock AI's helpers*).
