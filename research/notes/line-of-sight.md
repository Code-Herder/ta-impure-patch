# Line of sight — what lifts the fog, and what gates targeting

*2026-09-02. Reverse-engineered from the pristine exe (Ghidra decompiles of `0x4816A0`,
`0x465AC0`, `0x482270`, `0x4825B0`, `0x433130`, `0x40AA40`, `0x40AD80`, `0x40B7B0`,
`0x4089A0`, `0x406F80`; `tools/ghidra-scripts/DecompileTAFuncs.java`), then measured live
on two instances — the LOS byte map read straight out of the running process with
`tacli peek`, and every radius number below is a band counted in that map, not a
calculation. Struct names follow TADR's `tamem.h`. Written because a WarlordEx with
`SightDistance=1300` was firing at Skeeters sitting in fog, and neither half of that was
what it looked like.*

## Summary — the six things to know

1. **`SightDistance` saturates.** It is not a radius; it is a *table index*. In the only
   fogged mode a stock skirmish can produce, the ceiling is **8 cells = 256 world
   units**, and every unit with `SightDistance >= 256` — 350, 1300, anything — sees
   exactly that far.
2. **The ceiling is content, not code.** `gamedata/los.tdf`'s `numtables` is the clamp.
   Shipping a replacement in a `.ufo` moves it, up **and** down, with no byte patch:
   measured 128 / 256 / 288 world units at `numtables` 5 / 9 (stock) / 10.
3. **Stock ships one table it can never reach.** The clamp is `numtables - 1` and the
   accessor subtracts one again, so `TABLE9` is in the file and unused. Raising
   `numtables` to 10 unlocks it — +32 world units for a one-digit edit.
4. **Periodic target acquisition IS LOS-gated** — it scans a per-player *"enemies I can
   see"* list, not the world.
5. **Retaliation is NOT.** `FUN_00406F80` points every weapon slot at whoever just
   damaged the unit with no visibility test anywhere in the function. That is why ships
   shoot into fog, and it is stock — a plain `CORBATS` on an unarmed instance does it.
6. **Only one of SKIRMISH's three LOS stages actually fogs units.** `Permanent` (0) and
   `Circular` (2) both leave the whole LOS map lit; only `True` (1) restricts.

## 1. The setting — three stages, two bits

`SKIRMISH.GUI`'s `LineOfSight` gadget is `text=Permanent|True|Circular; stages=3`. Its
stage is decoded at game start into two ints — `SkirmishLineOfSight` (`TAdynmem+0x39221`)
and `SkirmishLOSType` (`TAdynmem+0x39225`) — which `FUN_00497180` folds into the
**`LosType` word at `TAdynmem+0x14281`** (`tamem.h`'s `LosType`), one bit each:

| bit | source | meaning |
|---|---|---|
| `0x1` | `SkirmishMapping` | terrain fog-of-exploration. **Clear = whole map pre-marked explored.** |
| `0x2` | `SkirmishLineOfSight` | **LOS restricts visibility.** Clear = every player's LOS map is pre-filled with `1` and never stamped. |
| `0x4` | `SkirmishLOSType` | which algorithm stamps LOS: **set = the `los.tdf` ray fan ("true"), clear = the `vismasks.gaf` circle ("circular")**. |
| `0x8` | — | dirty flag; the LOS writers clear it (`AND …,0xFFF7`) when the local player's map changes. |

Measured on a live game (`tacli peek fog1 '*0x511DE8+0x14281:2'`), mapped, one CORBATSX:

| stage | label | `LosType` | LOS map |
|---|---|---|---|
| 0 | Permanent | **12** | every cell lit — no unit fog |
| 1 | True | **14** | real; ray fan; band measured at ±8 cells |
| 2 | Circular | **8** | every cell lit — no unit fog |

**So stage 2 does not do what its label says.** It clears bit `0x2` along with bit `0x4`,
so the circle path is selected and then never runs. The vismask ceiling in §3 is
therefore unreachable from the skirmish screen; the bit pair `(0x2 set, 0x4 clear)` is
producible in principle — `FUN_00497180`'s lobby branch feeds the same three bits from
the `online.dll` config block — but that was not measured here.

## 2. Where LOS lives

`Game_SetLOSState` (`0x4816A0`) walks **ten player records at `TAdynmem+0x1B63`, stride
`0x14B`**. The fields this note uses:

| offset in the record | what |
|---|---|
| `+0x73` | controller kind — `1` human, `2` AI (retaliation and the finder both test it) |
| `+0x7C` | **the LOS byte map**, one byte per cell — a *counter*, incremented per stamping unit |
| `+0x80` / `+0x84` | its cols / rows (equal to `TAdynmem+0x14293` / `+0x14297`) |
| `+0x88` | its size in bytes |
| `+0x108 + n` | ally table — `0` means "player `n` is my enemy" |
| `+0x146` | this record's own player slot |

A unit points at its owner's record through **`UnitStruct+0x96`**. One LOS **cell is 32
world units** (every index is `worldCoord >> 5`); the row index is
`(worldZ - altitude/2) >> 5`, TA's usual isometric shear. The shared *explored* map is
separate — `TAdynmem+0x14273`, one word per cell, bit `TAdynmem+0x2A43` for the local
player — and is what `PositionInPlayerMapped` reads when LOS is off.

Reading it live is two `peek`s (slot 0 shown):

```bash
tools/tacli peek t1 '*0x511DE8+0x1B63+0x7C:4' '*0x511DE8+0x1B63+0x80:4'
tools/tacli peek t1 '0x<map>+<row*stride+col>:x41'      # a band across a unit
```

### Is this unit visible? — `UnitInPlayerLOS` (`0x465AC0`)

In order, and the order matters:

1. `unit+0x96 == playerRecord` → **true**. Your own units are always visible.
2. `cIsCloaked & 4` → **false**. Cloak wins over everything below.
3. Not `state & 0x200` (sonar) and altitude below sea level (`TAdynmem+0x1427F`) →
   **false**. An unsonared submerged unit is invisible however close it is.
4. Up to **four points** derived from the def's bounding-box offsets (`+0x15E/+0x166/
   +0x16E`, then `+0x176`, then `+0x17A/+0x17E`) are tested in turn; any one of them on a
   non-zero LOS cell → **true**. A big unit peeks out of fog before its centre does.
5. With LOS off, each of those points goes to `PositionInPlayerMapped` instead, and the
   final fallback reads the shared explored map.

## 3. The sight radius, and its two ceilings

Both stamping paths turn `SightDistance` (`UnitDefStruct+0x202`) into an **index**, and
both clamp it against the size of a data table. Neither uses it as a distance.

### True LOS — `gamedata/los.tdf`

`FUN_00482270` (add) and `FUN_00481D50` (remove) do:

```c
idx = SightDistance / 32;                       /* floor, >= 0            */
if (idx >= FUN_00433520(0x51E6A0) - 1)          /* count of loaded tables */
    idx = count - 1;
fan = FUN_00433500(&DAT_0051E6A0, idx);         /* element idx - 1 !      */
```

`FUN_00433130` fills that container from `gamedata/los.tdf`: `[TABLEINFO] numtables=N`,
then element `i` ← section `TABLE(i+1)` for `i = 0..N-1`. Each `lineK = count, x1,y1, …`
is one ray of cell offsets, mirrored into four quadrants at load. So element `idx-1` is
`TABLE(idx)`, whose comment is literally `// Radius of idx`:

> **radius in cells = min(SightDistance / 32, numtables − 1)**

Stock `numtables=9` → **8 cells = 256 world units**, and `TABLE9` is dead.

**Measured** (`scenarios/los-acquire-probe.json`, `--los 1`, a `CORBATSX` with
`SightDistance=1300`; the number is the lit band counted in the player's own LOS map):

| `los.tdf` shipped in a `.ufo` | lit band | radius |
|---|---|---|
| `numtables=5` (tables untouched) | cols −4..+4 | **128 wu** |
| stock — `numtables=9` | cols −8..+8 | **256 wu** |
| `numtables=10` + a dummy `TABLE10` | cols −9..+9 | **288 wu** |

That third row is the whole finding in one line: **the extra table unlocks `TABLE9`,
which stock already ships**, and nothing but a `.ufo` changed. Going past 9 needs new
`TABLE10…` ray geometry authored in the same format — not attempted here.

### Circular — `anims/vismasks.gaf`

The other branch blits a mask frame instead:

```c
frame = clamp(SightDistance / 32 - 5, 0, nframes - 1);
```

`nframes` is the frame count of sequence **`vismask`** in **`anims/vismasks.gaf`**, loaded
in `FUN_00429870` and cached at `TAdynmem+0x1485B`. That sequence has **ten** frames,
11×11 … 29×29 cells with offsets (5,5) … (14,14), so:

- frame 0 covers `SightDistance` 160–191, frame 9 covers **480 and up**;
- ceiling **14 cells = 448 world units**, and anything `>= 480` is the same circle.

Confirmed live only as far as the engine's own bookkeeping goes: at stage 2 the WarlordEx
stored **9** in `UnitStruct+0xF8` (the frame index) where at stage 1 it stored **123**
(true LOS keeps the unit's *height* there instead, `def+0x170 + altitude`, clamped to
255). The mask itself never reaches the map, because stage 2 also clears bit `0x2` (§1).

## 4. What targeting actually does with LOS

Four ways a weapon slot gets a target. **One of them checks LOS.**

| path | code | LOS-gated? |
|---|---|---|
| periodic acquisition | `FUN_004089A0` → `FUN_0040B7B0` → `FUN_0040AD80` | **yes** |
| retaliation on damage | `FUN_00406F80` | **no** |
| an explicit attack order | order state machine | no |
| radar-blip fallback | second list in `FUN_0040AD80` | needs radar; none in these tests |

### Acquisition — the per-player "enemies I can see" list

`FUN_0040AA40` rebuilds, for each player, a small set of vectors in a `0x10D`-byte record
at `DAT_005119C0[slot]`, **every 30 sim ticks** (`FUN_0040B2C0`, about a second at normal
speed). Walking every live unit, for each one that is not an ally:

```c
if (UnitInPlayerLOS(ctx, u) && !(u->state & 0x8000))  push(list@+0x05);   /* visible  */
if (u->state & 0x100)                                 push(list@+0x15);   /* on radar */
```

`FUN_0040AD80(ownerSlot, pos, radius, …)` then scans **that visible list** by squared
distance — no visibility test of its own, because the list is already the answer — and
only if it comes up empty *and* the player owns an active unit whose def flags carry
`0x400` does it fall back to the radar list.

Two details worth having:

- **The search radius is the weapon's range**, `FUN_0049ADF0(unit, slot)` =
  `WeaponStruct+0xDC` (peeked live: `810` for `CORE_BATSLASER`). `FUN_0040B7B0`'s other
  branch, the one that would search `SightDistance` instead, is **dead** — the finder has
  exactly one caller (`0x408B76`) and it always passes `1`.
- The candidate gate `(def+0x241 & 0x8000) || <owner is human/AI> || (SoftwareDebugMode &
  0x400)` is where **`+shootall`** lands; bLoBbY's "shootall default" patch is the
  `0F 84`→`0F 85` at `0x0040B8F5` inside it. It changes *eligibility*, never visibility.

### Retaliation — no gate at all

`FUN_00406F80(attacker, victim)` filters on stance, the def's `0x10010000` flags, the two
category masks (`def+0x23D`, `def+0x231`) and `UnitAutoAim_CheckUnitWeapon`, then calls
SetTarget on **every slot**. There is no LOS, no fog and no radar test in the function.
A unit shoots back at whatever hit it, seen or not — and because the acquisition port
*keeps* an existing valid target rather than re-choosing (`continue` in `FUN_004089A0`),
that target then sticks.

### Measured

`scenarios/warlordex-vs-fleet.json` at `--los 1`, sampling slot targets against distance:

- **WarlordEx** (armed, seven slots): targets at 274, 385, 445, 457, 482, 499, 676, 715,
  **728** world units, against a 256-unit LOS. Stock slots 0/1 and extended slots 3–6
  behaved identically.
- **Control — stock `CORBATS`, unarmed instance, no module code at all**: targets at
  **287 and 299**, same 256-unit LOS. So this is the engine, not `tagpu_weapons.c`.
- **Acquisition control** (`scenarios/los-acquire-probe.json` — four Skeeters parked at
  420 and 700, no orders, so nothing ever damages the ship): **no target at all** while
  they sit there, though all four are inside the 810 laser range. Every target it did
  take read a non-zero byte at its own LOS cell.

Retaliation is the whole difference between those two runs.

## 5. Consequences

- **Raising `SightDistance` above the ceiling is a no-op.** `tools/warlordex_content.py`
  asked for 1300 to "see everything it can reach"; it gets 256, exactly what 288 would
  buy. The comment there now says so.
- **Every stock unit out-ranges its eyes.** The stock Warlord sees 350 (→ 256) and shoots
  1250; that is normal TA, not a mod artefact. Long-range units genuinely need a spotter,
  and genuinely will still fire back at an unseen attacker.
- **For the extra-weapons module** this is inherited behaviour, not new: `my_Acquire` and
  `my_Retaliate` are call-for-call ports and neither adds a target source. The one real
  asymmetry is elsewhere — the engine's two "re-check this slot's target and drop it if
  it went bad" loops (`0x406482`, `0x40FE28`) are `slot < 3` and were **not** extended, so
  an extended slot holds a stale target a little longer than a stock one would.
- **A LOS change is sim-visible.** It moves what units acquire, so both peers of a
  multiplayer game need the same `los.tdf`.

## 6. Reproduction

```bash
tools/tacli launch fog1 --res 1024x768 && tools/tacli stop fog1
tools/tacli arm fog1 weapons.on
ln -s $PWD/scenarios/content/warlordex.ufo tagpu/instances/fog1/gamedir/
rm -f tagpu/instances/fog1/catalogue.json

tools/tacli scenario load fog1 los-acquire-probe --los 1 --restart
tools/tacli peek fog1 '*0x511DE8+0x14281:2'            # LosType: expect 14
tools/tacli peek fog1 '0x51E6A4:4' '0x51E6A8:4'        # ray tables: (end-begin)/16 = 9
tools/tacli peek fog1 '*0x511DE8+0x1B63+0x7C:4' '*0x511DE8+0x1B63+0x80:4'
# then dump a band of the map through the ship's cell and count the non-zero run
```

To move the ceiling, rebuild `los.tdf` with a different `numtables` and drop it in:

```bash
python3 tools/hpipack.py --extract tagpu/gamedir/totala1.hpi gamedata/los.tdf /tmp/
sed -i 's/numtables=9;/numtables=10;/' /tmp/los.tdf     # plus a TABLE10 section
python3 tools/hpipack.py /tmp/lostest.ufo gamedata/los.tdf=/tmp/los.tdf
cp /tmp/lostest.ufo tagpu/instances/fog1/gamedir/zz_lostest.ufo
```

The `.ufo` must be removed again afterwards — it is a sim change and it stays in the
instance until deleted.
