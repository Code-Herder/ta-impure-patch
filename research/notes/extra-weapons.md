# More than three weapons per unit — feasibility

*Static RE of everything in the pristine `TotalA.exe` that bakes in "a unit has
three weapons", and what it would take to lift that to a fixed larger number or
to a per-unit-type count read from the FBI. All VAs are for the pristine build
(ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). Decompiles and
scans via the shared Ghidra project (headless); the scan scripts are in
`tools/ghidra-scripts/` (see the last section).*

Evidence tags: **[BINARY-VERIFIED]** = instruction read from the Ghidra listing
for this build (VA quoted). **[DECOMPILE]** = read from the Ghidra decompile;
semantics inferred. **[CORPUS]** = layout or name from TADR's `tamem.h` /
Delphi structs or the symbol corpus.

## The short answer

**Both options are feasible, and they cost almost the same.** The engine never
allocates weapon state dynamically: every unit type carries exactly three
weapon-definition pointers, every live unit carries exactly three inline 28-byte
weapon-state slots, and the code addresses a slot either by walking those three
in a loop or by `unit + 4 + idx * 0x1C` with an `idx` that comes from a
**2-bit field** in the slot's own state byte. That 2-bit field is the only
thing that makes "exactly 4" cheaper than "exactly 10": four weapons fit the
field, ten do not. Everything else — storage, loops, COB script name tables,
FBI parsing, the network packet — has to be done either way.

So the recommendation is the **dynamic** variant: a compile-time capacity
(say 16) and a per-unit-type count read from `WeaponN=` keys in the FBI. A unit
with ten laser towers is then a data-only mod on top of that patch: ten
`WeaponN` keys, ten turret pieces in the 3DO, and `AimWeapon4..10` /
`FireWeapon4..10` / `QueryWeapon4..10` / `AimFromWeapon4..10` functions in the
COB (the Spring-engine naming, which the whole TA-on-Spring corpus already uses).

What it is *not*: a byte-tweak. Growing the structs in place is off the table
(the `UnitStruct` stride `0x118` is hard-coded at **133 sites in 73 functions**,
the `UnitDefStruct` stride `0x249` at **41 sites in 16 functions**
[BINARY-VERIFIED, scalar scan]). The realistic shape is TADR-style: side tables
owned by our DLL, two engine functions re-implemented in C, and roughly a dozen
five-byte detours at the places that compute a slot address or decode the
2-bit index. Multiplayer needs every peer patched (the fired-weapon packet
already carries the slot index as a full byte, so the wire format survives).

## What the engine actually does today

### Data model [CORPUS, cross-checked against the decompiles]

`UnitDefStruct` (one per unit type, stride `0x249`):

| Offset | Field | Set by |
|---|---|---|
| `0x1EE / 0x1F2 / 0x1F6` | `weapon1..3` → `WeaponStruct*` | FBI loader, keys `weapon1..3` |
| `0x231 / 0x235 / 0x239` | `w{pri,sec,spe}_badTargetCategory` mask pointers | FBI loader |
| `0x241` bit `0x10000` | "has any weapon" | FBI loader, from the three pointers |

A missing weapon is **not** NULL: the loader stores a pointer to
`Weapons[0]` (`TAdynmem + 0x2CF3`), the all-zero entry, and every consumer
tests `weapon->+0x10A != 0` for "real weapon" [DECOMPILE, `0x42CDDE..0x42CF04`].

`UnitStruct` (one per live unit, stride `0x118`): three inline slots at
`unit + 0x04`, `+0x20`, `+0x3C`, 28 bytes each. TADR's Delphi `TUnitWeapon` is
the accurate layout (the C `tamem.h` puts `Weapon1` at `0x10`, which is the
`p_Weapon` field of slot 0, not the slot start):

| Slot offset | Field | Notes |
|---|---|---|
| `+0x00` | `nTargetID` (u16) | unit index of target, or X of a ground target |
| `+0x02` | `nUsedSpot` (u16) | `0x8000` = target is a unit, else Z of ground target |
| `+0x04` | COB thread handle | preset to `0x4FD6F0` at unit creation; `StartScript` writes here |
| `+0x08` | aim-script result (u32) | zeroed before `AimN` is started; nonzero = aimed |
| `+0x0C` | `p_Weapon` | copied from the def at creation |
| `+0x10` | ZAngle | |
| `+0x14` | `nReloadTime` (u16) | ticks left |
| `+0x16 / +0x18` | heading / pitch (u16) | last aim solution |
| `+0x1A` | `cStock` | stockpile count |
| `+0x1B` | `cStateMask` | **bit0** aiming, **bit1** weapon present, **bits 2–3 = slot index**, **bit4** may acquire; bits 5–7: no reader found |

### The 2-bit slot index [BINARY-VERIFIED]

`UNITS_StartWeaponsScripts` (`0x49E070`) writes `(i & 3) << 2` into the state
byte of slot *i* [DECOMPILE]. From then on the index is **read back from that
byte**, never recomputed from the slot's position:

- `AutoAim` (`0x49E1A0`) looks up the aim-script name with
  `[EDX*4 + 0x509688]` where `EDX = state & 0xC` at `0x49E306`, `0x49E370`,
  `0x49E39D`.
- `UNITS_FireProjectile_0_3` (`0x49C9C0`) looks up the fire-script name with
  `state & 0xC` at `0x49CB86`, and reads the slot's heading with
  `unit + 0x1A + idx * 0x1C`. Same lookup in `UNITS_FireProjectile_1` at
  `0x49CD41` and `UNITS_FireProjectile_0` at `0x49CF65`.
- All four fire callbacks (`fire_callback0..3` at `0x49D580`, `0x49DB70`,
  `0x49DD60`, `0x49D9C0`) decode `state >> 2 & 3` two or three times each: to
  pick the `Query*`/`AimFrom*` script, and to fill the `WeapIdx` byte of the
  fired-weapon packet.

The two name tables are three entries long and immediately followed by their
own string bytes, so they cannot be extended in place:
`FirePrimary/FireSecondary/FireTertiary` pointers at `0x509678`,
`AimPrimary/AimSecondary/AimTertiary` at `0x509688` [BINARY-VERIFIED,
`.data` dump]. Index 3 in the Aim table would dereference `"AimT"` as a pointer.

The `Query*` and `AimFrom*` names are not in a global table at all: three
functions build a **three-entry local array on the stack** and index it with
the weapon index — `UNITS_CallAimScripts` (`0x43E2E0`),
`UNITS_QueryWeaponPosition` (`0x43E240`) and `FUN_0043E1E0` (called from
`InitProjectile` `0x49C740`) [DECOMPILE]. Index ≥ 3 reads a stray stack dword
as a `char*`.

### Every site that hard-codes three [BINARY-VERIFIED unless noted]

Found by (a) scalar scans for the def offsets `0x1EE/0x1F2/0x1F6/0x231/0x235/0x239`,
(b) a text scan for `ADD reg,0x1C` slot strides plus `[reg+0x10]`/`[reg+0x2C]`/`[reg+0x48]`
pointer loads, (c) xrefs of every weapon-related string, (d) callers of every
function found. Per-frame functions marked ●.

| Site | Function | What it does with "3" | Patch action |
|---|---|---|---|
| `0x42CDDE`–`0x42CF04` | FBI loader `FUN_0042BF40` | reads `weapon1..3`, `w*_badTargetCategory`, sets has-weapon flag | detour after the block (TADR hooks this loader at `0x42BF97` for its own extra keys); read `weapon4..N`, `wN_badTargetCategory` into the def side table |
| `0x42ADC5`, `0x42AE35`, `0x42AEAC` | `FUN_0042A8D0` | XORs the three weapon TDF CRCs into `CRC_weapons` (unit-sync handshake) | extend the XOR to N so patched peers agree; unpatched peers would report unit CRC errors anyway |
| `0x42B64C`, `0x42B7E5` | `FUN_0042B370` def copy | two 3-iteration copies | side table is per type, nothing to copy |
| `0x409930` | `FUN_00409730` AI valuation | scores a unit type by its 3 weapons | optional |
| `0x48606F`, `0x486293` | `UNITS_Create`, `UNITS_CreateFromNetwork` | preset 3 slot handles to `0x4FD6F0` | detour: also preset the side slots |
| `0x49E070` ● | `UNITS_StartWeaponsScripts` | loop `< 3`, writes the 2-bit index, copies def pointers, runs `Query*`/`AimFrom*`, computes `SetMaxReloadTime` | **re-implement in C** (40 lines) |
| `0x49E1A0` ● | `AutoAim` | loop `≤ 2`, 3× Aim-table lookup, per-slot target check + fire | **re-implement in C** (~120 lines; calls only exported-address helpers) |
| `0x49CB86`, `0x49CD41`, `0x49CF65` ● | `UNITS_FireProjectile_*` | Fire-table lookup by `state & 0xC`; heading by `idx*0x1C` from unit base | detour each lookup (3) to a helper that returns the name for the real index |
| `0x49D580`, `0x49DB70`, `0x49DD60`, `0x49D9C0` ● | `fire_callback0..3` | `state>>2&3` ×2–3 each; packet `WeapIdx = idx & 3` | detour each decode (≈10 sites) to `SlotIndex(unit, slot)` |
| `0x49D270` ● | `ReceiveWeaponFired` | `slot = unit + 4 + pkt[0x23]*0x1C` | detour the address computation to `SlotPtr(unit, idx)` |
| `0x43E2E0`, `0x43E240`, `0x43E1E0` ● | `UNITS_CallAimScripts`, `UNITS_QueryWeaponPosition`, `FUN_0043E1E0` | 3-entry stack name arrays | **replace** (each is ~15 lines: query script, add piece position) |
| `0x40719D` | `FUN_00406F80` retaliation on damage | loop `< 3` over slots, `badTargetCategory[idx]` | detour loop bound, or re-implement (60 lines) |
| `0x408B94` | `FUN_004089A0` periodic acquisition (AI/idle) | loop of 3 | same |
| `0x4897E0` | first weapon with bit1 set | unrolled 3 | replace (5 lines) |
| `0x4898B0` | clear target for slot `n`, `3` = all | recursion over 0,1,2 | replace |
| `0x49ABB0`, `0x49ADF0`, `0x49D120`, `0x48A060`, `0x48A0A0`, `0x48A0F0`, `0x48A190`, `0x48A1E0`, `0x40B7B0` | index-based helpers | `unit + 4 + idx*0x1C` (or `+0x10`) | detour the address computation (one `LEA`/`IMUL` each) |
| `0x4022xx`, `0x4035xx`, `0x4037xx`, `0x4139xx` | attack/guard order state machines (no Ghidra function bodies) | pass an order-held index to `CheckUnitWeapon` | none — index-based |
| `0x46AC51`–`0x46AC83` | HUD panel `FUN_0046A860` | 3 reload bars | optional; leave at 3 |
| `0x487625`, `0x487A51` | savegame load/save `FUN_00487080` / `FUN_004876C0` | 3 slots × 0x18-byte records, bit-packed state | optional; extra slots start cold after a load |
| `0x488570` | `UNITS_GiveUnit` | packet `0x14` carries 3 `cStock` bytes | optional (stockpile weapons beyond slot 3 lose their stock on give) |
| `0x43F223`, `0x43F25B`, `0x43E54F`, `0x43F1B7`, `0x43F292` | order-type selection, cursor | look at weapon 1 / weapon 2 only | none |

Where the tick comes from: `FUN_0048AD30` walks every player's unit array and
calls `AutoAim` **only for units owned by a local human or local AI**
(`player + 0x73 ∈ {1, 2}`) [DECOMPILE]. Remote units are driven by the
`0x10 UNIT_START_SCRIPT` (method *index*, not name) and `0x0D WEAPON_FIRED`
packets. So a peer without the patch does not aim the extra weapons at all; it
would only *receive* a fired packet with `WeapIdx ≥ 3` and index past its three
slots into `UnitOrders` — memory corruption, not a graceful miss.

### Storage constraints [BINARY-VERIFIED scalar scan]

| Constant | Meaning | Sites | Functions |
|---|---|---|---|
| `0x118` | `UnitStruct` stride | 133 | 73 |
| `0x249` | `UnitDefStruct` stride | 41 | 16 |

Growing either struct in place means rewriting every one of those and every
`unit + 0x58..0x117` field offset behind the slots. TADR never does this; it
grows *pools* (projectiles, explosions, weapon types) by re-pointing the
absolute addresses and bound constants embedded in the instruction stream, and
it grows *per-type data* with a side table keyed by unit-type id
(`UnitDefExtensions.cpp`). The same two patterns cover us:

- **Def side table**: `ExtraDef[unitTypeId] = { count, WeaponStruct* w[CAP-3],
  uint32* badMask[CAP-3] }`, filled from the FBI-loader detour. Keyed by
  `UnitDefStruct::UnitTypeID` (`+0x21E`) or the array index (the def array base
  is `TAdynmem + 0x1439B`, stride `0x249`).
- **Unit side table**: `ExtraSlots[UnitInGameIndex][CAP-3]` of the same 28-byte
  slot struct, sized from `MaxUnitLimit` at start (`TAdynmem + 0x37EEA`, u16, so
  worst case 6553 × 13 × 28 ≈ 2.4 MB at CAP 16). Slot 0..2 stay inline;
  `SlotPtr(unit, i)` returns `unit + 4 + i*0x1C` for `i < 3`, else
  `&ExtraSlots[unit->UnitInGameIndex][i-3]`. Reset when a unit is created
  (the create-function detours).

The unit-index key is recycled when a unit dies, which is fine because creation
resets the row before anything reads it.

## Option A — a fixed number

**Exactly four**: the 2-bit index still works, so the fire callbacks and
`UNITS_FireProjectile_*` keep their decode; only the *tables* they index change
(re-point the six absolute `0x509678`/`0x509688` operands at four-entry tables in
our DLL). Everything else in the inventory is still required: side storage for
the fourth slot and def pointer, the loop rewrites, the three stack-array
replacements, the FBI keys, the CRC, the create-time preset. Saving relative to
dynamic: about ten detour sites. Value: low — a fourth weapon is a marginal
feature and the user's target is ten.

**Exactly N > 4** (a constant): identical work to dynamic, minus a `count`
field; the loops run to N and rely on "missing weapon = pointer to
`Weapons[0]`" to skip empty slots, exactly like stock does for a unit with one
weapon. There is no reason to prefer this over a count.

## Option B — dynamic (per-unit-type count, fixed capacity)

`CAP` is a compile-time capacity (16 fits the 5 bits available if the index is
kept in the state byte; 32 if bits 5–7 are proven free); `def->count` is what
the loader found. All loops become `for (i = 0; i < count; i++)`; `count` is
stock 3 for every unit whose FBI has no `Weapon4`, so **stock units run exactly
the stock loops** — that is the regression guard.

Two ways to carry the index once it no longer fits two bits, both local to
code we already detour:

1. **Derive it**: every site that decodes the index has both the unit and the
   slot pointer in registers (`fire_callback*(unit, slot, …)`,
   `AutoAim` loop, `FireProjectile(slot, unit, …)`), so
   `SlotIndex(unit, slot)` is `(slot - unit - 4) / 0x1C` for inline slots and
   `3 + (slot - ExtraSlots[u]) / 0x1C` otherwise. No state-byte format change;
   the 2-bit field keeps holding `i & 3` for anything we do not touch.
2. **Widen it in place**: use bits 5–7 of the state byte as the high part.
   Savegame packing only copies bits 0–4 and no reader of bits 5–7 was found
   (`TEST byte ptr [reg+0x1F/0x3B/0x57]` scan — one explicit mask, `0x2`, plus
   register masks in `FUN_004897E0` and `UNITS_GiveUnit`), but "not found" is
   not "proven"; (1) needs no such proof.

Go with (1).

### Script and data conventions for authors

- **FBI**: `Weapon4=` … `WeaponN=`, `w4_badTargetCategory=` … (stock uses
  `wpri_/wsec_/wspe_`; keep those for 1–3).
- **COB**: `AimWeapon4`, `FireWeapon4`, `QueryWeapon4`, `AimFromWeapon4` …
  (Spring's names; `scriptor` accepts any function name). Slots 1–3 keep
  `AimPrimary` etc. so existing COBs are untouched. `TargetCleared(n)` and
  `RockUnit` already take or ignore the index.
- **Weapon count on the wire**: `WEAPON_FIRED_0D.WeapIdx` is a byte at
  `+0x23` [CORPUS `TWeaponFiredMessage`, matches the receiver at `0x49D270`].
  TADR's extended-weapon-ID build uses a longer packet (`WeaponID` becomes a
  dword and every later field shifts); if we ever run alongside `tdraw.dll` the
  receiver detour has to read the index at the shifted offset. Our stack is
  standalone, so this is a note, not a blocker.

## Making it optional

Three layers. Layer 2 is the one that carries the design weight, and it is
the one that rests on claims that have only been read off decompiles so far;
the checklist in the next section is what turns those claims into facts.

### Layer 1 — arming, off by default

A flag file `tagpu_weapons.on` next to the exe, checked once at
`DLL_PROCESS_ATTACH`, the same convention as `tagpu_suppress.on` and
`tagpu_tracer.on`. Absent: the module returns before touching memory, and the
process is byte-identical to an unpatched one. Present: the module installs.
Because each `tacli` instance owns its own game directory, the switch is per
launch and per instance; it cannot be toggled mid-game, because the detours
and side tables are installed at attach and live state accumulates in them.

Arming is **all-or-nothing**: every site is byte-match guarded, all sites are
checked *before* the first write, and a single mismatch leaves every site
untouched and logs `weapons: disarmed (site 0x…, expected … got …)`. A
half-patched engine is worse than an unpatched one.

### Layer 2 — stock units run stock code

With the module armed, a unit type whose FBI has no `Weapon4` key must behave
exactly as it does today, down to the tick. Two mechanisms:

- **Replaced functions take a trampoline back for `count <= 3`.** The entry
  detour on `AutoAim`, `UNITS_StartWeaponsScripts`, the three name helpers and
  (if replaced rather than bound-patched) the retaliation and acquisition loops
  lands in a C wrapper that does
  `if (ExtraDef[unit->def].count <= 3) return orig(unit, …);` where `orig` is
  the trampoline: the stolen prologue bytes followed by a jump to
  `function + 5`, in the `VirtualAlloc`'d stub. That is the unmodified engine
  function minus the instructions we overwrote. Stock units therefore run
  Cavedog's aim loop, Cavedog's script-start sequence and Cavedog's name
  lookup, not our re-implementation; the rewrite's risk is confined to units
  that need it.
- **Mid-function splices compute what the engine computed.** The ~25 five-byte
  detours at slot-address computations and index decodes cannot take a
  trampoline, they sit inside functions. Their stub calls `SlotPtr(unit, idx)`
  or `SlotIndex(unit, slot)`; for `idx < 3` these return exactly the
  `unit + 4 + idx * 0x1C` (or `+ 0x10`) the engine would have produced, so the
  only difference for a stock unit is a few extra instructions per call. This
  equivalence is arithmetic, not behaviour, and it still has to be measured,
  not argued (assertion 3 below).
- The re-pointed `Aim*`/`Fire*` tables begin with the same three stock strings,
  so indices 0–2 resolve to the same script names as before.
- The state byte's 2-bit index is written by the engine itself for stock units
  (the original `UNITS_StartWeaponsScripts` runs). For `count > 3` our code
  writes `i & 3` there for compatibility and never reads it back; the real
  index is derived from the slot pointer.

Consequence, and the property that makes the switch safe: **armed plus stock
content is tick-identical to unarmed.** That is a testable claim about
replays and multiplayer, and it is tested, not assumed (assertions 3 and 4).

### Layer 3 — content lives apart

The N-weapon unit ships as its own archive or loose `units/` files. Patch on,
content absent: nothing changes. Content present, patch off: the stock loader
ignores unknown FBI keys and the unit has its first three weapons.

### Multiplayer: who computes what, and the guard

From `FUN_0048AD30` [DECOMPILE], `AutoAim` runs only for units owned by a local
human or local AI (`player + 0x73 ∈ {1, 2}`). Remote units' weapons are driven
by two packets: `0x10 UNIT_START_SCRIPT`, which carries the COB **method index**
(`FUN_00456200` looks the name up first), and `0x0D WEAPON_FIRED`, whose
`WeapIdx` is a full byte at `+0x23`. So two armed peers with identical COB
files (already a unit-sync requirement) agree on which script a fired packet
means, and the receiver indexes the side slot for `WeapIdx ≥ 3`.

The guard for **mismatched** peers is the unit-sync handshake. `CRC_weapons`
(`def + 0x146`) is XOR-folded into `CRC_all` (`+0x142`) by
`UnitInfo_CalcScriptCRC` (`0x42A610`) before the first sub-2 `0x1A UNIT_DATA`
packet [CORPUS]. An armed peer folds `weapon4..N` in; an unarmed peer does not;
so exactly the unit types that carry `Weapon4` disagree, and every other type
still agrees. **What TA then does with a disagreeing unit — refuse to start,
disable that unit, or only print "You have CRC errors on N units!" — is not
established** (assertion 8). If it is warn-only, the guard is not a guard, and
we must add our own: either block the start, or have the armed side disable
its extra weapons for that game. The failure mode being guarded against is not
cosmetic: an unarmed receiver handling `WeapIdx ≥ 3` indexes past its three
slots into `UnitOrders`.

## Assertions that must hold before this ships

Each row is a claim made above from static reading. None is proven live yet.
The A/B harness is the existing one: the same JSON scenario
(`scenarios/shootall-ab.json`, `scenarios/200v200.json`) in two `tacli`
instances, armed and unarmed, with periodic roster dumps compared.

| # | Assertion | Why it matters | How to verify |
|---|---|---|---|
| 1 | Unarmed (no flag file): not one byte of the image is written | the switch is real | at attach and at first frame, checksum every page the module would touch and compare with the pristine file bytes; `tagpu.log` says `weapons: disarmed` |
| 2 | Arming is all-or-nothing | a half-patched engine corrupts silently | debug build with one site's expected bytes deliberately wrong: module reports disarmed and every other site is still pristine |
| 3 | Armed + stock content: the sim is tick-identical to unarmed | layer 2's whole point; replay and MP safety | A/B scenario run; diff roster dumps (position, HP, kills, reload, target) at fixed ticks; any drift is a bug in a mid-function stub |
| 4 | Armed + stock content: the C re-implementations are never entered | same | hit counters in every C path stay 0 across a full stock skirmish, AI included |
| 5 | Trampolines are correct for every replaced function | wrong stolen-byte length = crash on the fast path | disassembly check that no relocated instruction is IP-relative (a `jmp`/`call` in the first 5 bytes) — `DisasmWindow.java` on each entry |
| 6 | An N-weapon unit fires all N, each through its own `Aim/Fire/Query/AimFrom` script | the feature | test unit with `Weapon4..N`; log script starts per slot and projectile spawns per weapon type |
| 7 | Remote peer: `0x10` arrives as method index and `0x0D` `WeapIdx ≥ 3` lands in the side slot | MP correctness between armed peers | two armed `tacli` instances in a LAN game; receiver-side log of `WeapIdx` and the slot it resolved to |
| 8 | Mismatched peers: the lobby reports CRC errors for exactly the `Weapon4` unit types, and nothing else | the optionality guard | one armed, one unarmed instance; read the lobby messages and the unit-sync result per unit type |
| 9 | What TA does on a unit CRC mismatch (refuse start / disable unit / warn only) | decides whether we need our own guard | same test, then attempt to start; if it starts, build the extra guard before anything else |
| 10 | `AutoAim` is never executed for remote-owned units | confirms the ownership model the MP story rests on | log owner state in the `AutoAim` wrapper during the LAN game; remote units must never appear |
| 11 | Save then load with an N-weapon unit: no crash, side slots are re-initialised | known gap is benign | save mid-fight, load, confirm weapons 4..N resume (the load path must reset side slots; the savegame loader does not call `UNITS_StartWeaponsScripts`) |
| 12 | Give/share an N-weapon unit: no crash | known gap is benign | share the unit in the LAN game; stock beyond slot 3 may be lost, nothing else |
| 13 | No caller passes `idx ≥ count` for a stock unit | guards the four order-state-machine regions Ghidra has no function bodies for | `SlotPtr` asserts `idx < count` and logs violations; run a long AI skirmish and a unit-heavy scenario |
| 14 | The mid-function stubs cost nothing measurable | ~25 splices on per-tick paths | tick rate of `200v200.json` armed vs unarmed |
| 15 | Unarmed peers never receive `WeapIdx ≥ 3` | the corruption case | only true if 8/9 give a real guard; otherwise our own guard must make it true |

## Cost and risk summary

| Work item | Fixed 4 | Dynamic |
|---|---|---|
| Arming gate, all-or-nothing install, `count <= 3` trampolines | ✔ | ✔ |
| Def + unit side tables, create-time reset | ✔ | ✔ |
| FBI loader detour (`weaponN`, `wN_badTargetCategory`) | ✔ | ✔ |
| Re-implement `UNITS_StartWeaponsScripts`, `AutoAim` | ✔ | ✔ |
| Replace the three stack-array name helpers | ✔ | ✔ |
| Name tables for `Aim*`/`Fire*` | 6 operand re-points | same, or handled inside the helpers |
| Slot-address detours in the index-based helpers (~12) | ✔ | ✔ |
| Index-decode detours in fire callbacks + `FireProjectile_*` (~13) | — | ✔ |
| Retaliation / acquisition loops (2) | ✔ | ✔ |
| `CRC_weapons` extension | ✔ | ✔ |
| HUD bars, savegame, give-unit stock | optional | optional |

Risk lives in three places: (1) a missed `unit + 4 + idx*0x1C` computation
somewhere the scans did not reach — the four order-state-machine regions with
no Ghidra function bodies are the ones to re-scan by hand; (2) the CRC/unit-sync
handshake, which decides whether two patched peers agree a unit is the same
unit; (3) `AutoAim` is the hottest weapon function (every local unit, every
tick) and a C re-implementation must be tick-for-tick identical for stock units
or replays diverge. The mitigation for (3) is the `count == 3` fast path
calling the original function, enabled until the rewrite is proven on a stock
skirmish.

## Suggested proof-of-concept path

0. **Settle the guard first**: assertions 8 and 9 need no patch at all — a
   loose `units/*.fbi` override that changes one unit's `weapon3` on one of two
   `tacli` instances reproduces a unit CRC mismatch today. Whether TA refuses,
   disables or only warns decides whether the design needs its own guard.
1. **Read-only**: a `tacli` roster query that dumps the three slots of a
   selected unit (state byte, reload, target) — confirms the slot layout live
   and is the A/B oracle for assertions 3 and 4 from then on.
2. **Names first**: replace the three stack-array helpers and re-point the two
   tables at DLL-side tables with `CAP` entries, behind the flag file and the
   all-or-nothing install. Assertions 1–5 pass before anything else is added.
3. **Storage + loops**: side tables, create-time reset, loader detour, the
   loop rewrites, slot-address detours. Test with a stock unit given a
   `Weapon4=` in a loose `units/*.fbi` override and a COB with `AimWeapon4` —
   a weapon that should fire and does not is a targeting-loop miss; one that
   crashes is an address-computation miss.
4. **Index decode**: the fire-callback / `FireProjectile_*` detours. Now
   `Weapon5+` works.
5. **Ten laser towers**: the actual unit — 3DO with ten turret pieces, COB with
   the ten function quads, FBI with ten keys.

## Tooling added for this survey

All in `tools/ghidra-scripts/`, run with the usual headless form
(`-process TotalA.exe.pristine -noanalysis -postScript X.java args…`):

| Script | Args | What |
|---|---|---|
| `WeaponSurvey.java` | `out, scalars(csv hex), string-regex, data-addrs(csv)` | instructions with matching scalar operands; defined strings matching the regex with their code xrefs (follows pointer tables one level); xrefs to data addresses |
| `WeaponSurvey2.java` | `out, regex;;regex…` | instruction-text regex scan grouped by function |
| `CallersOf.java` | `out, funcs(csv)` | code xrefs to each function entry |
| `DisasmWindow.java` | `out, addr:before:after,…` | disassembly windows around addresses (for code Ghidra has no function for) |
| `DecompileTAFuncs.java` | `funcs(csv), out` | (pre-existing) decompile a list of functions |
