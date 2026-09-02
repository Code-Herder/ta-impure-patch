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
| `0x42ADC5`, `0x42AE35`, `0x42AEAC` | `FUN_0042A8D0` | XORs the three weapon TDF CRCs into `CRC_weapons` (unit-sync handshake) | **not done.** Extend the XOR to N so armed peers agree and mismatched ones disagree; what the lobby then does is assertion 8, not a given |
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
| `0x4039BE` in `FUN_004038A0` | attack-**ground** order, fire branch | unrolled: ground target on slots 0 and 1 only | splice `ground.order`: do slot 1, then every side slot (snag 9) |
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

The intended guard for **mismatched** peers is the unit-sync handshake, and it is
worth being exact about what exists today. `CRC_weapons` (`def + 0x146`) is
XOR-folded into `CRC_all` (`+0x142`) by `UnitInfo_CalcScriptCRC` (`0x42A610`)
before the first sub-2 `0x1A UNIT_DATA` packet [CORPUS], and `CRC_weapons` itself
is built by `FUN_0042A8D0`, which folds in one weapon TDF CRC at each of three
hard-coded sites (`0x42ADC5`, `0x42AE35`, `0x42AEAC`) — one per stock slot.

**The module does not extend that fold, so the guard does not exist yet.** A unit
type's CRC is today identical whether it carries four extra weapons or none. Two
consequences, and only the second is serious:

- Between two *armed* peers, two builds of the same unit that differ only beyond
  `Weapon3` hash the same. In practice unit-sync already requires identical
  files, so this is a hazard rather than an observed failure.
- Between an armed and an unarmed peer, **nothing disagrees at all**. Extending
  the XOR to `weapon4..N` is what would make exactly the `Weapon4`-carrying types
  disagree while every other type still agrees — that is the design, not the
  current behaviour.

Even once extended, the guard is only as good as what TA does with a disagreeing
unit, and **that is not established** (assertion 8): refuse to start, disable the
type, or warn only. The often-quoted "You have CRC errors on N units!" text does
**not** exist in the binary — the only sync-related strings are `+syncerr`,
`SYNCHING` and `Synchronization complete` — so whatever the lobby says comes from
a GUI file, not a hard-coded message. If it turns out to be warn-only, the CRC is
not a guard at all and we must add our own: either block the start, or have the
armed side disable its extra weapons for that game.

The failure mode being guarded against is not cosmetic: an unarmed receiver
handling `WeapIdx ≥ 3` indexes past its three slots into `UnitOrders`. The
extension and the answer to assertion 8 were blocked on DirectPlay until
2026-09-02; native DirectPlay now works under wine, so both are merely
outstanding (see "Multiplayer — untested, and why").

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
| 8 | Mismatched peers: the lobby reports CRC errors for exactly the `Weapon4` unit types, and nothing else | the optionality guard | **needs the `CRC_weapons` extension first — it is not implemented, so today nothing disagrees**; then one armed, one unarmed instance, and read the lobby messages and the unit-sync result per unit type |
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

## Implementation — landed 2026-09-02

Option B as decided (per-type count from `Weapon4..N`, capacity 16), as the
module `tagpu/ddraw/src/tagpu_weapons.c` (contract in
`inc/tagpu_weapons.h`), initialised from `DllMain` after the render passes.
Everything below was measured on the pristine 3.1 build under wine with `tacli`.

### Shape

| Piece | What it is |
|---|---|
| Gate | `tagpu_weapons.on` next to the exe at attach; absent = not one byte written (the oracle still works). `tacli arm <inst> weapons.on` before launch. |
| Install | 20 entry hooks, 20 mid-function splices, 4 in-place byte patches, all byte-matched **before** the first write; one mismatch = `weapons: DISARMED` and nothing touched. Stubs and trampolines live in one `VirtualAlloc`'d RWX pool (1.6 KB used). |
| Entry hooks (trampoline for stock) | `UNITS_StartWeaponsScripts`, `AutoAim`, the three name helpers, retaliation `0x406F80`, acquisition `0x4089A0` (per *batch*: the original runs unless a unit in the cursor's batch is extended), `0x4897E0`, `0x4898B0`, `0x489800` (an index helper the survey missed — the allocator's per-slot "enable"), `0x48A060/0A0/0F0/160`, `0x49ADF0`, `0x48A190`, `CheckUnitWeapon`, `Trajectory3` (also missed by the survey: it reads `unit+0x10+idx*0x1C`), `0x49D120`, and the def copy `0x42B370` (keeps the side record with the type it describes). |
| Splices | loader `0x42CEF2` (reads `weaponN` / `wN_badTargetCategory`), `WEAPON_FIRED` receiver `0x49D364` (clamps `WeapIdx >= count` to slot 0 and logs), the three `FireProjectile_*` name lookups, eleven `state>>2&3` decodes in the four fire callbacks, two in the target finder `0x40B7B0`, one in the target-position helper `0x48A1E0`. |
| Byte patches | the three `FireProjectile_*` heading loads (`mov si,[ebp+ecx*4+0x1a]` → `mov si,[edi+0x16]`, the slot pointer is in a register) and one `and al,3` after a spliced decode. |
| Side tables | def records keyed by def array index (the game-start loader compacts the array, numbers it, *then* runs the FBI loader per final slot, so the index is stable; the record also stores the def pointer and answers "stock" on a mismatch); unit side rows `[units][13]` sized from the live unit array and reset by the module's own `StartWeaponsScripts` at creation (which every create path, savegame load included, goes through). |
| Slot index | derived from pointers (`SlotIndex(unit, slot)`); the 2-bit field still holds `i & 3`. |
| Names | slots 0–2 use the engine's own strings and tables; 3+ are `AimWeaponN` / `FireWeaponN` / `QueryWeaponN` / `AimFromWeaponN` (1-based). |
| Oracle | `tagpu_weapons.trigger` → `tagpu_weapons.json`; `tacli weapons <inst> [idx…]` prints every slot of every unit (state, weapon, target, reload, heading, pitch, stock, aim result, thread), the arming state, the C-path hit counters and projectile launches per slot. Works unarmed, which makes the unarmed instance the control. Four counters are diagnostics rather than coverage: `violation` and `mismatch` must stay 0 (assertions 3 and 13), `cob_full` must stay 0 (snag 10 — nonzero means the unit ran out of COB threads and some slot is aiming from piece 0), and `hold_fire` counts shots declined because the barrel had not slewed on target yet, which is working as intended. |

### Content tooling (no COB compiler needed)

- `tools/hpipack.py` — HAPI writer *and* reader (`--list`, `--extract`). Finding
  the hard way: the engine only mounts an archive whose last 36 bytes are
  `Copyright 1997 Cavedog Entertainment` (details in file-formats.md §5).
- `tools/cobalias.py` — add script names pointing at existing code (an alias
  shares the aim script's `signal`/`set-signal-mask`, so two aliased slots kill
  each other's aim thread and only the last starter fires — a content artifact).
- `tools/cobclone.py` — clone `AimPrimary`/`FirePrimary`/`QueryPrimary`/
  `AimFromPrimary` once per extra weapon with relocated jumps and a private
  signal bit, so N slots aim concurrently through one turret.
- `tools/extra_weapons_fixture.py` — builds `scenarios/content/wpn-test.ufo` (the
  archive itself is gitignored like every `*.ufo`; run the script once per checkout)
  (ARMPW4: a Peewee with `Weapon4`; ARMLLT10: an LLT with `Weapon1` + `Weapon4..10`)
  from the game's own files. Scenarios `wpn-peewee4.json`, `wpn-llt10.json`.
- `tools/ta3domod` — read/edit/write `.3do`. `--stretch PIECE:AXIS:CUT:AMOUNT`
  inserts hull at a cut plane (vertices past it slide out, a child past it moves
  bodily, a child before it is entered with the plane rebased into its frame);
  `--graft SRC:PIECE:DST:X,Y,Z:NAME` lifts a turret subtree off another unit,
  renaming descendants so the COB's names stay unique. `ta3domod export` is
  ta3do's glTF export plus `asset.extras.ta3do.atlas` — for every packed tile,
  the GAF entry and archive it came from, its pixel rect and its UV rect — so an
  externally edited mesh can be written back as a `.3do` with texture names
  intact. Needs `tools/ta3do`, which is still on `worktree-3do_exporter`.
- `tools/warlordex_content.py` — builds `scenarios/content/warlordex.ufo`:
  CORBATSX "WarlordEx", the Warlord with 50 model units spliced into its
  midbody and four CORLLT turrets on the deck that opens, with
  `Aim/Fire/AimFrom/QueryWeapon4..7` written straight as bytecode (one turret
  and one signal bit each, modelled on CORLLT's aim script) and a `Create` that
  also hides the four new flares. New code is appended and `Create`'s entry
  repointed at a relocated copy, so every absolute jump already in the file
  still lands where it did.
- Loose `units/*.fbi` overrides are rejected by the engine (the type vanishes);
  ship content as a `.ufo`. `tacli scenario` caches the type list per instance in
  `catalogue.json`; delete it after changing archives or the validation refuses
  a type the game does have.

### Snags — every one that cost a run, building WarlordEx

Seven weapons on a hand-built unit, and the first six live runs all failed for a
different reason. None of them was the engine module: it reported `n=7`,
`violation=0`, `mismatch=0` throughout. Every one was content.

**1. `AimWeaponN` must not wait for the turn — the arrival test belongs in the
module.** The most expensive one, three passes to get right, and every wrong
answer was a plausible-looking script. Measured on
`scenarios/warlordex-vs-fleet.json`, one 40-ship run each, shots on slots 4..7
(weapon `CORE_BATSLASER`, range 810):

| `AimWeaponN` body | slot 4..7 shots | verdict |
|---|---|---|
| `signal`, `set-signal-mask`, turn, wait-for-turn, return 1 — Cavedog's own shape | 0 2 0 6 | starves the COB pool; barely fires |
| turn, wait-for-turn, return 1 (`track`) | 32 32 34 23 | starves it worse, and poisons the aim origin |
| `turn-now` both axes, return 1 (`snap`) | 48 10 10 5 | on target, but the turret teleports |
| **turn, return 1 (`slew`) + the module's arrival gate** | **19 18 25 19** | **even, slews, and aligned** |

Read the first three rows with snag 10: **any aim script that waits holds one of
the unit's eight COB threads for the whole slew**, and four extra turrets cannot
each have one. That is the single cause behind both failures. Cavedog's `signal`
pair is not decoration — it kills the previous instance, so a waiting script that
omits it leaks a thread per restart and exhausts the pool in seconds; `track`
does exactly that, which is why it looks best on shot count and is the worst of
the four in play (starved slots share one bogus heading and stop tracking). And
`full` keeps the pair but still parks four threads in `wait-for-turn`, leaving
nothing for the fire scripts and `SmokeUnit`.

An earlier version of this note blamed the pair itself and said the mechanism was
unexplained. It also floated a double-start, which is wrong: stock `AutoAim`
(`0x49E1A0`) makes the same two calls the port does, and the second, `0x456200`,
is a bare `HAPI_BroadcastMessage` of packet `0x10` that returns immediately unless
`TAdynmem+0x2A44` says the game is networked [DECOMPILE], so in a skirmish only
one instance runs. Both guesses are retired: it is thread exhaustion, and the
`signal` pair is the *cure* for a waiting script, not the disease.

So the shipped script does not wait, and does not need the pair either. It issues
the turn and returns inside the tick — a `turn` keeps running without a thread
once issued — and the module supplies the guarantee the wait used to
(`barrel_on_target()`, snag 10). Dropping the wait *without* that gate was the
second wrong answer: the slot then reports aimed the instant the turn starts and
the beam leaves a barrel that has not swung round, which reads in game as "the
lasers don't come from the tip of the barrel". The model is not at fault —
CORLLT's `gun` geometry ends at z = −23.62 and the `flare` piece `QueryWeaponN`
hands back sits at −23.86, a quarter of a unit past the tip.

**2. A short-ranged extra weapon needs a scenario that brings the enemy to it.**
`CORE_LIGHTLASER` reaches 300; `ARM_ROY` reaches 660, the stock Warlord's own
`COR_BATS` 1250. A fleet parks at arm's length and the new battery never fires a
shot — the run looks exactly like a broken weapon, and a WarlordEx duel against
two stock Warlords was won 2-0 with the four lasers silent throughout. Check
`range` on both sides before blaming the content. WarlordEx now carries
`CORE_BATSLASER`, the same 810-range High Energy Laser its own tri-barrel turret
fires, so the battery reaches as far as the ship it is bolted to.

**3. `energypershot` is a silent gate.** `CORE_LIGHTLASER` costs 10 energy a
shot, and a scenario with `clear_existing: true` takes the starting commander
away — with it the player's whole energy *storage*, which the engine recomputes
from owned units every tick. Level pinned at 0, and the extra slots simply never
fire while the stock ones (which cost nothing) carry on. Give the owner
generation and storage; the fixture parks two fusions and three energy stores
offshore of the fight.

**4. `MaxDamage` above 32767 kills the unit on spawn.** A live unit's current
health is `short Health` at `unit+0x108` (`tamem_ghidra.h:1107`), signed. 61400
wraps to −4136 and the ship dies on the frame it appears.
`warlordex_content.py` clamps and says so.

**5. Inherit the FBI through the merged archive view, not `totala1.hpi`.** Core
Contingency ships its own `CORBATS.FBI` in `ccdata.ccx`, and that is the Warlord
the game loads. Deriving from `totala1.hpi` gives a unit built on stats the
player never sees.

**6. Grafted model pieces need unique names, and `Create` must hide the new
flares.** The COB addresses pieces by name, so a second `flare` from a donor
turret collides; `ta3domod --graft` suffixes the whole subtree. And a muzzle
flare that no `Create` hides is a cone stuck on the deck for the unit's life.

**7. TA's two order groups, and what a scenario can reach.** A scenario `stance`
(`hold` / `manoeuvre` / `roam`) is the *movement* group only; there is no
fire-state key. The fire group is an FBI property — `NoAutoFire=1` — so a
target that must not shoot back is a unit variant, not a scenario setting.
`warlordex.ufo` ships `ARMROYH` "Crusader (Hold)" for exactly that.

**8. Housekeeping that bites.** Delete the instance's `catalogue.json` after
every archive change or `scenario load` refuses a type the game does have.
`--los 0` (permanent line of sight) takes through the registry, but `--mapping`
does not — toggle `Mapping` on the SKIRMISH screen with `tacli ui set Mapping 1`
and use `scenario apply` rather than `load`, since `load` relaunches and loses it.
Without both, the whole fight happens inside a black circle.

**9. An attack-ground order drives slots 0 and 1 and nothing else — engine, not
content.** Reported from play: ordered onto a spot, the WarlordEx shells it with
the stock laser and cannon while all four new turrets sit idle. `FUN_004038A0`,
the attack order's state machine, unrolls the fire branch at `0x40399C` by hand —
`ClearTargetN 0`, `ClearTargetN 1`, `SetGroundTarget 0`, `SetGroundTarget 1` — and
that is the whole of it [BINARY-VERIFIED]. Weapon 3 is reached only by the sibling
branch at `0x40396B`, which the order takes when its held weapon index is exactly
2; nothing sets that for an extended unit. So the extra slots were never given the
spot at all: `tacli weapons` showed `tgt=0 spot=0x8000` on every one of them while
0 and 1 fired. Note the *unit*-target order does not have this problem for a
different reason — it hands one slot the target (`order+0x36`, from `FirstWeapon`)
and lets the rest reach it through acquisition, which the module already ports.
Ground targets have no acquisition path, so there is nothing to fall back on.

Fixed with a 9-byte splice, `ground.order`, over the second `SetGroundTarget` call
at `0x4039BE`: it does that call and then gives every side slot the same spot on
the same terms (clear may-acquire first, exactly as `ClearTargetN` did for 0 and
1). Stock units are untouched — the callback returns after slot 1 when
`count <= 3`. The `ground` hit counter in `tacli weapons` is the live proof it
ran; the fixture is `scenarios/warlordex-groundattack.json`, one ship, no enemy
inside weapon range, so every shot slots 4..7 fire came from the order.

**10. A unit gets eight COB threads, and running out is silent.**
`COBEngine_AllocThread` (`0x4B08C0`) scans a fixed **eight** `0xA4`-byte records
at `cob+0x1C` and returns `-1` when they are all busy [BINARY-VERIFIED]; the
running-thread count sits at `cob+0x53C`, immediately after the array, so the
eight is a hard layout constant, not a tunable. Two callers matter:

- `COBEngine_QueryScript` (`0x4B0BC0` → `0x4B0C40`) opens with that allocation
  and, on `-1`, **returns without touching the caller's out-parameter**. No error,
  no log. `UNITS_CallAimScripts` initialises the piece to `-1`, asks
  `AimFromWeaponN`, gets silence, falls back to `QueryWeaponN`, gets silence, and
  uses piece **0** — the hull's first piece. Every starved slot then computes the
  same aim origin, so several turrets suddenly share one heading and pitch and
  stop tracking, while the ones that got a thread carry on. That is exactly what
  "one turret won't move to aim, and sometimes it starts working again" looks
  like from the outside.
- any aim script that `wait-for-turn`s holds a thread for the whole slew.

A warship spends most of the eight before the extra weapons arrive: `AimPrimary`,
`AimSecondary`, the `RestoreAfterDelay` each of them starts, `SmokeUnit` once
damaged, and the fire scripts (Cavedog's flare is `show; sleep 150; hide`). Four
more waiting aim scripts do not fit. Worse, without `signal` they *accumulate* —
Cavedog's `signal`/`set-signal-mask` pair exists to kill the previous instance,
so a script that waits and does not signal leaks a thread per restart and
exhausts the pool in seconds. That is the real reason snag 1's `full` and `track`
both fail, and it subsumes the guess left there: the pair is not optional for a
long-lived script, and a script that needs no pair is one that does not wait.

Two fixes, both in `tagpu_weapons.c`:

1. **Cache the piece.** `AimFromWeaponN`/`QueryWeaponN` return a constant, so
   `slot_piece()` asks once per unit *type* and remembers it in the def record
   (`pc_aimfrom` / `pc_query`). After the first call the aim origin is a pointer
   chase that cannot fail, and the per-tick query traffic disappears. Slots 0–2
   still go through the engine every tick — a stock `Query*` may legitimately
   answer a different barrel each call.
2. **Move the arrival test out of the script.** The extended slots run an aim
   script that returns inside its own tick (`AIM_STYLE = "slew"`), so no thread
   is held; a `turn` keeps running once issued without one. `my_AutoAim` then
   re-solves those slots every tick instead of latching state bit 0 (which is
   only cleared on target loss, and would otherwise freeze the turret on its
   first solution), re-issuing the turn only when the answer moves, and
   `barrel_on_target()` withholds the shot until the muzzle piece really points
   at the target — the muzzle rotates with the gun, so `mount → muzzle` versus
   `mount → target` is the whole test, in yaw, to `AIM_TOLERANCE` (1024, 5.6°).

`tacli weapons` reports both: `cob_full` counts aim origins that fell back to
piece 0 (must stay 0) and `hold_fire` counts shots declined mid-slew.

### Assertions — status

| # | Status | Evidence |
|---|---|---|
| 1 | **holds** | unarmed launches log nothing and leave every site pristine (the install path is never entered); the oracle reads the stock slots exactly as the note's layout says (states `0x12/0x14/0x18`, weapon in slot 0, thread preset `0x4FD6F0`). |
| 2 | by construction, not exercised | `verify_all()` runs over all 44 sites before any write; a deliberate-mismatch debug run is still to do. |
| 3 | **holds (live)** | armed + stock content through two `shootall` fights and an AI game: `stock_splice` = 8410 stub executions on the stock path, `mismatch` = 0 (the pointer-derived index agreed with the engine's 2-bit field every time), no crash across ~25 000 frames. A roster diff at fixed ticks between the armed and unarmed instances was not done (the two instances are not tick-aligned); the stub equivalence is proven directly instead. |
| 4 | **holds** | every C-path counter (`start autoaim names retaliate acquire helpers splice ground hold_fire`) stays 0 with stock content; only `loader` moves, once per unit type. |
| 5 | **holds** | all 20 stolen prologues checked by hand against objdump for IP-relative code (none); every trampoline was executed on the stock path. |
| 6 | **holds** | ARMLLT10: ten slots (`n=10`), each side slot holds `ARM_LIGHTLASER`, the target, an aim result and state `0x1F/0x13/0x17/0x1B` (`i & 3` in bits 2–3); `fires by slot: 0 3 4 5 6 7 8 9` all launched projectiles; the aim solutions of slots 3–9 equalled the stock slot-0 solution at the same tick (logged side by side during development). ARMPW4: slot 3 launched 60 projectiles in a fight. |
| 7–10, 15 | **untested** | needs two instances in one game; see "Multiplayer — untested, and why" below and [networking-lobbies](networking-lobbies.md). The only in-module guard is the receiver clamping `WeapIdx >= count` to slot 0. |
| 11 | holds by construction | savegame load creates units through `UNITS_Create` → the module's `StartWeaponsScripts` → side rows reset; not exercised live. |
| 12 | not exercised | give-unit needs a LAN game. |
| 13 | **holds so far** | `violation` = 0 in every run (AI skirmish, both fights, the extended scenarios). |
| 14 | not measured | no tick-rate comparison yet. |

### Multiplayer — untested, and why

Background on TA's network model (the lockstep packet stream, the DirectPlay
providers, what TA Forever and the demo recorder tunnel) is in
[Networking, Lobbies & Multiplayer](networking-lobbies.md); this section only
covers what the extra-weapons work needs from it and what stopped the test.

Every multiplayer assertion (7 remote fire packets, 8/9 the unit-CRC handshake, 10
`AutoAim` ownership, 12 give-unit, 15 unarmed peers) needs two `tacli` instances in
one TCP/IP game over loopback. That was attempted on 2026-09-02 and does not work on
this machine yet. What happened, so nobody re-derives it:

1. **The provider screen does not crash for TCP/IP.** The ta-drive skill records that
   `SELPROV`'s `SELECT` kills the game (`Access Violation at 0023:00000000`). With
   row 0, *Internet TCP/IP Connection For DirectPlay*, selected, `SELECT` goes to
   `TCP.GUI` ("Enter TCP address (leave blank to search)") with no crash. The
   recorded crash is therefore specific to the other providers (IPX was the one
   tried before), not to DirectPlay as such.
2. **Blank address**: `OK` or Enter answers "Invalid TCP/IP Address" and returns to
   `SELPROV`. **`127.0.0.1`**: `OK` reaches `SELGAME.GUI` behind an "Updating..."
   box, then bounces back to `SELPROV` two seconds later; no session list, no host
   button ever became clickable. (The `tacli ui fill` of the address field once kept
   only `1` — retype and read back what the field reports.)
3. **Root cause**: wine's DirectPlay TCP/IP service provider does not implement
   hosting. On wine 9.0 it implements nothing at all — launched by hand with
   `WINEDEBUG=-all,+dplay,+dplayx,+dpwsockx` the log shows
   `DPWSCB_EnumSessions … stub`, `NS_SendSessionRequestBroadcast : not all data
   fields are correct`, `DPWSCB_CloseEx … stub`, `DPWSCB_ShutdownEx … stub`.
   Nothing TA-side is at fault.
4. **The earlier conclusion that a newer wine or native DirectPlay would fix it
   was half right, and the half that matters is the bad half.** Measured
   2026-09-02 with `tools/dptest`, a standalone 32-bit DirectPlay probe that
   asks `dplayx`/`dpwsockx` for exactly what TA asks (see
   [Networking, Lobbies & Multiplayer](networking-lobbies.md) §"DirectPlay under
   Wine — measured" for the full write-up):

   | Call | wine 9.0 | wine 11.0 (Proton Experimental) |
   |---|---|---|
   | `EnumSessions` | `DPERR_UNSUPPORTED` | `DP_OK` |
   | `Open(DPOPEN_CREATE)` — host | `DPERR_UNSUPPORTED` | `DPERR_UNSUPPORTED` |

   `dpwsockx` was rewritten upstream between 2023-10 and 2024-11 and has been
   unchanged from `wine-10.0` through `master`; `DPWSCB_Open` still begins
   `if ( data->bCreate ) { FIXME( "session creation is not yet supported\n" ); return DPERR_UNSUPPORTED; }`.
   **Wine can join a DirectPlay session but cannot create one**, so upgrading
   wine does not help.
5. **Unblocked the same day with native DirectPlay.** Microsoft's own
   `dplayx` + `dpwsockx` + `dplaysvr.exe`, in front of wine's builtins, make
   hosting work on the stock **wine 9.0** the instances already use — host,
   join, players and game messages all `DP_OK` in `tools/dptest`, in both
   `win32` and `win64` prefixes. `tools/dpinstall.sh <prefix>` installs them.
   Two traps: the two **EXEs must be overridden by name** as well
   (`dplaysvr.exe,dpnsvr.exe=n`) or `Open` hangs silently on wine's stub
   dplaysvr, and a **stale `dplaysvr.exe` holding UDP 47624** across prefixes
   makes the next host fail `DPERR_GENERIC` — `pkill -x dplaysvr.exe` first.
   Provenance and the Authenticode verification are in
   [Networking, Lobbies & Multiplayer](networking-lobbies.md).

   **So assertions 7-10, 12 and 15 are no longer blocked** — they are simply not
   run yet. What remains is `tacli` work (per-launch DirectPlay overrides
   alongside the hard-coded `ddraw=n,b`) and then the two-instance
   `scenarios/wpn-llt10.json` test.
6. Until that test runs, the multiplayer claims in this note (remote units driven by the `0x10`
   and `0x0D` packets, the CRC handshake as the mismatched-peer guard) rest on the
   static reading only. In particular the "You have CRC errors" lobby message is not
   a string in the binary, so what the lobby does on a unit-CRC mismatch is still
   unknown; if it turns out to be warn-only, the module needs its own guard before
   anyone plays an armed build against an unarmed one.

### Known gaps (unchanged from the plan)

`CRC_weapons` is not extended for `weapon4..N`, so an armed and an unarmed peer
agree on the CRC of a unit they disagree about — the mismatched-peer guard is
absent, not merely weak (§Multiplayer); now testable — native DirectPlay works
under wine as of 2026-09-02. The
HUD still shows three reload bars; savegames do not persist slots 4+ (they restart cold); `UNITS_GiveUnit`
carries three stock bytes. A unit whose *only* weapons are 4+ (no `Weapon1`)
gets the def's has-weapon flag cleared by the engine after our detour; keep
`Weapon1` populated.
