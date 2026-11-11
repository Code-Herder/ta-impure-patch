# Data keys the engine reads

The TDF getters, the parsers that call them, and every key each parser reads: the unit FBI, the move classes, weapons, features, the map's OTA and the sidedata. The inventory tasim's coverage is measured against.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

**How this page was made** [DISASSEMBLED 2026-11-11]: every call to the getters below was listed
from `objdump -d -M intel` of `pristine/TotalA.exe.pristine`, and for each call the key string it
pushes, the default and the store that follows were read. A key is listed with its destination
only where that store was read; "not traced" marks the rest. **What this page does not yet say**
is which code *reads* each field back — which keys the simulation consumes and how — the question
tasim's coverage needs (*Not yet mapped*, at the end).

## The getters

A TDF section object holds a sorted vector of `{key*, value*}` pairs from `+0x19` (begin) to
`+0x1D` (end); every getter bisects it with `_stricmp 0x4F8A70`, so **keys are case-insensitive**.

| Getter | Convention | Returns | Callers |
|---|---|---|---|
| `0x4C46C0` | `thiscall(section; key, int default)`, `ret 8` | `atoi` (`0x4E4F70`) of the value, else the default | 287 |
| `0x4C4760` | `thiscall(section; key, double default)`, `ret 0xC` | `atof` (`0x4E4560`) on the x87 stack, else the default | 36 |
| `0x4C48C0` | `thiscall(section; out, key, size, default*)`, `ret 0x10` | copies at most size − 1 characters (`0x4E4760`) and returns 1, else copies the default ([memory](memory.html), *the TDF string reader*) | 120 |
| `0x4C4800` | `thiscall(section; int* out, key, int default)`, `ret 0xC` | `atof` × 65536.0 (`0x4FDC20`) truncated (`0x4E43A0`) into `*out`: 16.16 fixed point; returns `out` | 8, all in the FBI parser |
| `0x4C4630` | `thiscall(section; key)`, `ret 4` | the value's string pointer, or NULL | 6 |
| `0x4C45E0` | `thiscall(section; n)`, `ret 4` | the n-th pair: walks a section whose keys are data | 2 (a weapon's damage section) |
| `0x4C58A0` | `ret 0x14`; `(section, out, key, size, default)` [INFERRED from its call sites] | the key with the language prefix `0x51FDC0` [INFERRED: a localized lookup; its fallback order not traced] | 8 |

Caller counts are `call rel32` instructions in `.text`.

## The parsers

| Parser | File | Callers |
|---|---|---|
| `0x42BF40` | a unit's FBI `[UNITINFO]` → its UnitDef (stride `0x249`, `ebp` = the def) | `0x42D269`, `0x42D722` (the unit-data load `0x42D2E0`, loader thread) |
| `0x440340` | a move class → a `0x20`-byte record, initialised by `0x4402E0` | `0x42D39B` (moveinfo's classes, 32 records at `0x512358..0x512758`), `0x42CD51` (a unit with no `movementclass`: its own FBI section) |
| `0x42E440` | a weapon TDF → the weapon record (stock layout `main+0x2CF3 + ID·0x115`; the raised limits move it, [weapons](weapons.html)) | `0x42E3E7` in the weapon load `0x42E310` |
| `0x4224B0` | a feature TDF → its FeatureDef (256 bytes, `main+0x14253`-indexed) | `0x422026`, `0x422E90`, `0x422FA4`, `0x42304C`, `0x4230F4`, `0x4231DE`, `0x424DE1` |
| `0x422EA0` | a feature's successors | `0x424ECD`, `0x4918D9` |
| `0x42A8D0` | the menu-time unit pre-scan (the CRC and incompatible-units check) | `0x42BD29` |
| `0x42D2E0` | the unit-data load, with the sidedata `CANBUILD` lists | `0x4918CA` |
| `0x42DCF0` | the download TDFs ([the GUI](ui.html), *The download menus' records*) | `0x4918CF` |
| `0x435DA0` | the map's OTA | `0x43528F`, `0x435A5A`, `0x435BDF`, `0x435C16`, `0x435D02` |
| `0x436C30`, `0x48E010`, `0x436860` | mission and campaign scripts | keys not listed here |
| `0x4AD350`, `0x4AD890`, `0x4ADC70`, `0x4AEAC0` | `.GUI` files | [gui-gadgets](../gui-gadgets.html) |

## Unit FBI keys — `0x42BF40`

Integers and floats default to 0 and strings to "" (`0x5119B8`) unless a default is given.
`fx` = 16.16 through `0x4C4800`; `dbl` = through `0x4C4760`; `int` = through `0x4C46C0`.

- **Strings.** `unitname` (def `+0x20`, 32 bytes), `name` and `description` (both through
  `0x4C58A0`; `name` at def `+0x00`, `description` at `+0x40`), `defaultmissiontype`,
  `wpri_badTargetCategory` (its mask stored at `+0x231`), `wsec_badTargetCategory`,
  `wspe_badTargetCategory`, `noChaseCategory` (mask at `+0x23D`), `objectname`, `category`,
  `soundcategory`, `corpse` (→ `+0x1BC` through `0x422E40`), `movementclass` (→ `0x440420` → the class
  pointer `+0x1B6`), `weapon1`..`weapon3` (→ `+0x1EE`, `+0x1F2`, `+0x1F6` through `0x49E5B0`;
  [extra weapons](../extra-weapons.html) adds more), `explodeas` (→ `+0x220`), `selfdestructas`
  (→ `+0x224`), `YardMap` (→ `+0x14E`, [units](units.html)); `selfdestructcountdown` read raw
  through `0x4C4630`.
- **Costs and economy.** `buildcostenergy` and `buildcostmetal` (int, stored as floats `+0x186`,
  `+0x18A`); dbl, stored as floats: `energymake` `+0x1C2`, `energyuse` `+0x1C6`, `metalmake`
  `+0x1CA`, `extractsmetal` `+0x1CE`, `windgenerator` `+0x1D2`, `tidalgenerator` `+0x1D6`,
  `energystorage` `+0x1E2`, `metalstorage` `+0x1E6`; `makesmetal` int, byte `+0x22D`.
- **Motion.** fx: `maxvelocity` `+0x192`, `brakerate` `+0x19A`, `acceleration` `+0x19E`,
  `bankscale` `+0x1A2` (default 1.0), `pitchscale` `+0x1A6`, `damagemodifier` `+0x1AA`, `moverate1`
  `+0x1AE`, `moverate2` `+0x1B2`. int: `turnrate` (word `+0x1BA`), `waterline` (byte `+0x22C`),
  `transportsize` (byte `+0x22A`), `transportcapacity` (byte `+0x22B`), `cruisealt` (word `+0x21C`),
  `maneuverleashlength` (word `+0x214`), `attackrunlength` (word `+0x216`), `kamikazedistance`
  (word `+0x218`).
- **Build and health.** `buildtime` (dword `+0x1EA`), `workertime` (word `+0x1FE`), `healtime`
  (word `+0x200`), `maxdamage` (dword `+0x1FA`), `builddistance` (word `+0x212`), `buildangle`
  (word `+0x210`), `bmcode` (byte `+0x22F`; 0 is a building).
- **Sensors.** words: `sightdistance` `+0x202`, `radardistance` `+0x204`, `sonardistance` `+0x206`,
  `mincloakdistance` `+0x208`, `radardistancejam` `+0x20A`, `sonardistancejam` `+0x20C`. `cloakcost`
  (int, stored as a float, `+0x1DA`) and `cloakcostmoving` (`+0x1DE`).
- **Other ints.** `sortbias` (word `+0x21A`); `standingmoveorder` (default **2**, def `+0x241` bits
  0–1) and `standingfireorder` (default **2**, bits 2–3).
- **Flags in def `+0x241`**, one key a bit (each read masked to bit 0 and shifted in):
  `init_cloaked` 4, `downloadable` 5, `builder` 6, `zbuffer` 7, `stealth` 8, `isairbase` 9,
  `istargetingupgrade` 10, `canfly` 11, `canhover` 12, `teleporter` 13, `hidedamage` 14, `shootme`
  15, `armoredstate` 17, `activatewhenbuilt` 18, `floater` 19, `upright` 20, `amphibious` 21,
  `isfeature` 24, `noshadow` 25, `immunetoparalyzer` 26, `hoverattack` 27, `kamikaze` 28,
  `antiweapons` 29, `digger` 30. Bit 31 is the orders-page bit ([orders](orders.html)).
- **Flags in def `+0x245`**: `mobilestandorders` 0, `firestandorders` 1, `onoffable` 2, `canstop` 3,
  `canattack` 4, `canguard` 5, `canpatrol` 6, `canmove` 7, `canload` 8, `canreclamate` 10,
  `canresurrect` 11, `cancapture` 12, `candgun` 14, `norestrict` 15, `showplayername` 17,
  `commander` 18, `cantbetransported` 19. **Bit 9 has no key**: the loader copies bit 10 into it
  (`0x42CA3B..0x42CA4F`); the resolver reads it as the repair capability
  ([orders](orders.html), *What each resolver mode answers*). **Bit 13 has no key**: it is set when
  `cloakcost` (`+0x1DA`) is above 0 (`0x42CA5A..0x42CA99`) [INFERRED: CanCloak].
- **Movement**, through `0x440340` from the named class or the unit's own section: `FootPrintX`
  (word, record `+4`), `FootPrintZ` (`+6`), `maxwaterdepth` (`+8`, default 10000), `minwaterdepth`
  (`+0xA`, default −10000), `maxslope` (byte `+0xC`, default 255), `badslope` (`+0xD`, default half of
  `maxslope`), `maxwaterslope` (`+0xE`, default 255), `badwaterslope` (`+0xF`, default half of
  `maxwaterslope`); the defaults are the record's values set by `0x4402E0`. It then clamps
  `maxslope` ≤ `maxwaterslope`, `badslope` ≤ `maxslope` and `badwaterslope` ≤ `maxwaterslope`
  (`0x4403F4..0x440418`). The unit gets `+4`, `+6`, `+8`, `+0xA`, `+0xC`, `+0xE` at def `+0x14A`,
  `+0x14C`, `+0x1BE`, `+0x1C0`, `+0x228`, `+0x229` (`0x42CD5D..0x42CD97`); `badslope` and
  `badwaterslope` stay in the class record, which `+0x1B6` points at when a class is named.
- **Read only by the pre-scan `0x42A8D0`**: `side`, `ai_weight`, `ai_limit`, `wacky`, `Version`,
  `Copyright`; it also reads `unitname`, `name`, `objectname`, the two costs, `norestrict`,
  `weapon1`..`weapon3`, `explodeas` and `selfdestructas`.
- **Sidedata**: `canbuild1`..`canbuildN` under `[CANBUILD]` (the format `canbuild%d`, `0x503ED8`,
  at `0x42DA14`).

## Weapon TDF keys — `0x42E440`

The record's fields and `+0x111`'s bits, with how each stored value is converted, are
[weapons](weapons.html), *What the loader writes into the record*; what the parser reads:

- `ID` (int, default −1; selects the record), `name`.
- dbl: `weaponvelocity` (`+0x68`) and `startvelocity` (`+0x6C`), both × 65536/30 (`0x4FD240`),
  `weaponacceleration` (`+0x70`; × 65536/900, `0x4FD248`), `reloadtime` (word `+0xE4`; × 30,
  `0x4FD250`: seconds to ticks), `energypershot` (float `+0xC0`), `metalpershot` (float `+0xC4`),
  `edgeeffectiveness` (float `+0xD8`), `minbarrelangle` (float `+0xC8`), `shakeduration` (`+0xD0`);
  and stored as words after a conversion not traced here: `weapontimer` `+0xE6`, `turnrate` `+0xE8`,
  `burstrate` `+0xEC`, `duration` `+0xF0`, `randomdecay` `+0xF2`, `smokedelay` `+0xFA`, `flighttime`
  `+0xFC`, `holdtime` `+0xFE`.
- int: `range` (dword `+0xDC`, default `0x7FFF`), `coverage` (`+0xE0`), `areaofeffect` (word
  `+0xD6`), `burst` (`+0xEA`), `sprayangle` (`+0xEE`), `firestarter` (byte `+0x10B`), `rendertype`
  (`+0x10C`), `color` (`+0x10D`), `color2` (`+0x10E`), `accuracy` (word `+0x104`), `tolerance`
  (`+0x106`), `pitchtolerance` (`+0x108`), `shakemagnitude` (`+0xCC`), `default` (the damage, word
  `+0xD4`), and the damage sub-section walked by index (`0x4C45E0`; its keys are unit names).
- Flags in `+0x111`: `lineofsight` 0, `ballistic` 1, `shellweapon` 2, `beamweapon` 3, `vlaunch` 4,
  `meteor` 5, `noradar` 6, `paralyzer` 7, `dropped` 8, `startsmoke` 9, `endsmoke` 10,
  `soundtrigger` 11, `guidance` 12, `tracks` 13, `unitsonly` 14, `groundbounce` 15, `waterweapon`
  16, `toairweapon` 17, `smoketrail` 18, `turret` 19, `selfprop` 20, `propeller` 21, `noexplode` 22,
  `burnblow` 23, `twophase` 24, `cruise` 25, `commandfire` 26, `noautorange` 27, `stockpile` 28,
  `targetable` 29, `interceptor` 30.
- Strings: `model`, `explosiongaf`, `explosionart`, `lavaexplosiongaf`, `lavaexplosionart`,
  `waterexplosiongaf`, `waterexplosionart`, `soundstart`, `soundhit`, `soundwater`.

## Feature keys — `0x4224B0`

Defaults 0 and "". `Description`, `footprintx` (word `+0x94`), `footprintz` (`+0x96`), `height`
(byte `+0xFA`), `object`, `filename`, `seqname`, `seqnameshad`, `seqnameburn`, `seqnameburnshad`,
`seqnamedie`, `seqnamedieshad`, `seqnamereclamate`, `seqnamereclamateshad`, `spreadchance` (byte
`+0xFB`), `reproduce` (`+0xFC`), `reproducearea` (`+0xFD`), `metal` (int, as a float `+0xF0`),
`energy` (int, as a float `+0xEC`), `damage` (word `+0xEA`), `sparktime` (dbl, word `+0xE8`),
`burnweapon`. Flags in the word `+0xFE`: `animating` 1, `animtrans` 2, `shadtrans` 3, `flamable` 4,
`geothermal` 5, `blocking` 6, `reclaimable` 7, `autoreclaimable` 8 (default 1), `indestructible` 9,
`nodisplayinfo` 10, `nodrawundergray` 11. The successors, `0x422EA0`: `featuredead`,
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

## Keys Impure adds

Read by the DLL, not the exe ([data keys](../modules/data-keys.html)):

- Unit FBI: `weapon4`..`weapon10` and `w4_badTargetCategory`..`w10_badTargetCategory`
  (`tagpu_weapons.c`); `VeterancyThresholds`, `VeterancyAccuracyBuffRate`,
  `TransportedExplodeAs`, `TransportedSelfDestructAs` and `PreviewPieces` (`tagpu_datakeys.c`).
- Weapon TDF: `nottoair`, `nottounderwater`, `surfacefire`, `notoverwater`, `notoverland`,
  `nomapweaponalert` (`tagpu_datakeys.c`).

## Not yet mapped

- **Who reads each field** — which of these keys the simulation consumes, where and how. This is
  the coverage tasim needs, and it is open for every section above.
- The `.GUI` parsers' and the mission parsers' keys in full (listed by parser only).
- `0x4C58A0`'s fallback order when the language-prefixed key is missing.
- The destinations of the unit FBI's remaining string keys (`defaultmissiontype`, `objectname`,
  `category`, `soundcategory`, the secondary and special `badTargetCategory` masks) and of the
  weapon TDF's strings.
- `ai_weight` and `ai_limit` are read by the pre-scan only; how the AI profile's `weight` and
  `limit` commands use a unit's values was not traced ([sim](sim.html), *Difficulty and the AI
  handicap*).
