# Weapons, damage and effects

Weapon decisions and IDs, damage, veterancy, area damage, projectiles, the effect pools and the nano spray.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### The nano spray: two emitters, fifteen order handlers [DISASSEMBLED 2026-09-29]

What the recorder's rule for owner-only effects rests on (*Decisions taken* in the
[completeness note](../tadr-port/demo-recorder-completeness.md#decisions-taken)): the nanolathe
spray is made by two emitters, called only from fifteen order handlers, which run only on the
machine that simulates the unit. Established by whole-image scans (E8, E9, Jcc and absolute
dwords at every byte offset) and recursive descent from each handler. The particle object, its
layer and its draw are in [effects §7](../effects.md); the nano class's boxes, bursts, step and dot
in *The nano particle*, below.

**The emitters** are `0x4720D0(point3, box6, layer)` and `0x472200(box6, point3, layer)`,
stdcall, `ret 0xC`, 297-byte bodies that differ only in which argument they expand: the forward
one copies its point into a degenerate box (`0x4720D3..0x472108`) and initialises the particle
from the point to the box; the reverse one expands its second argument and initialises from the
box to the point (`0x472203..0x4722A9`). Neither makes a particle while the byte `0x51E608` is
nonzero (`0x472112..0x47211D`) or when the pool `0x470EB0(0x51E610, 0x4C)` is empty
(`0x472126`). The object's vtable is `0x4FD5B8`. Its init, slot `+0x18` = `0x473B50`, copies the
two boxes to `obj+0x1C` and `obj+0x34`, sets its end tick to GameTime + 1 and spawns at once
(`0x473D50`, called at `0x473D39`). The spawn draws the CRT `rand 0x4E4870`, never the sim RNG
`0x4B6C30`, so an emission changes no simulation state. `0x4732E0` appends the object to the
layer's vector at `[main+0x38D77] + layer·16` (`0x4721EA`, reverse `0x47231A`); before it, an
emitter whose layer already holds more than its cap (`0x190` = 400 in stock, the operand the raised
limits rewrite: *The raised effect pools*) destroys the vector's front object through its vtable
slot 0 and shifts the rest down by one (the trim `0x47219F..0x4721DA`, reverse
`0x4722CF..0x47230A`, DISASSEMBLED 2026-10-12). The particle itself comes from the pool
`0x51E610`, which every live particle emitter shares (1000 objects of `0x4C` in stock, built by
`0x470A90(1000, 0x4C)` at `0x471C80..0x471C8C`; 204 800 under the raised limits): its alloc
`0x470EB0` returns NULL once used reaches capacity (`0x470EB9..0x470EBB`), and on NULL the emitter
makes no particle (`0x47213E..0x472163`, the skip to `0x4721EF`; DISASSEMBLED 2026-10-12). So the
order of the emissions of one step decides their order in the layer -- the order they are drawn
in, and which is destroyed first -- and, when the pool runs out within the step, which emission
makes no particle; nothing else. Both are what a recording's spray sessions change when they put
the sprays they imply after their step's other events ([the lossy stream](recorder.md#the-demo-recorders-lossy-stream-what-a-held-byte-meets-on-a-puppet-and-at-a-takeover-disassembled-2026-10-11)).
Neither entry has an absolute reference; the 17 callers, all passing layer 6, are the handlers
below.

**Copied multiplayer spray bounds [DISASSEMBLED 2026-10-05].** The forward
emitter reads its point argument at `0x4720D3`, expands its three coordinates on
the stack at `0x4720DE..0x4720F8`, reads the layer-table pointer at `0x47210C`,
and copies the low layer byte into object `+0xC` at `0x472142..0x472149`.
After the object's init call at `0x472179`, `0x47217C` sign-extends the **low
sixteen bits** of the original layer argument; `0x472181` scales by sixteen and
`0x472188` reads layer `+4` without an index bound. The constructor `0x471D90`
allocates `0xA0` bytes (`0x471D92..0x471D97`), loops ten rows (`0x471DAD`,
`0x471DBD..0x471DC1`) and publishes the table at `main+0x38D77` (`0x471DCB`).
The reverse emitter reads its point at `0x472203` and expands it at
`0x47220E..0x47222C`. The copied root decoder requires a 48-byte payload,
boolean reverse flag and signed low layer in 0..9 before publishing a batch;
the unused high layer word remains byte-exact. This matches the existing
skirmish replay bound. The recorder reuses its query/handler/point credit
invariant; an uncredited multiplayer spray fails contributions incomplete.
No new emitter patch or engine state write is installed.

**The handlers.** Each is referenced once, at its `.rdata` record's `+4`, and runs only from the
heap copy (*`0x512344` — the order-descriptor array*). The action is the record's rank there
[INFERRED from the comparator and the registrations; not read live]. The spray starts at
`0x43E400` of the order's owner `order+0xE`, except in SelfRepair, where it starts at the
repairer `order+0x16`: that order belongs to the aircraft being repaired, and VTOL_Landing
`0x4118E0` makes it with the landing pad as its target (`0x411EBF..0x411EE1`, the only reference
to "SELFREPAIR", `0x411ECF`).

| Handler | Order (action) | Emits at | Box | Emits when | Runs every |
|---|---|---|---|---|---|
| `0x402430` | SelfRepair (`0x28`) | `0x4025AE` | A of the aircraft | the step `0x41BD10` at `0x402518` returned 1 | tick |
| `0x402640` | BuildingBuild (`0x0C`) | `0x402ABD` | A of the nanoframe `order+0x16` | `0x41BA60` at `0x402A09` returned 1 | tick |
| `0x403A20` | MobileBuild (`0x19`) | `0x403ECC` | B of the target | `0x41BA60` at `0x403E43` | tick |
| `0x403F70` | HelpBuild (`0x17`) | `0x4041C2` | B | `0x41BA60` at `0x404139` | tick |
| `0x413D80` | VTOL_MobileBuild (`0x36`) | `0x4142B6` | B | `0x41BA60` at `0x414235` | tick |
| `0x414380` | VTOL_HelpBuild (`0x33`) | `0x4146D7` | B | `0x41BA60` at `0x414656` | tick |
| `0x405300` | RepairUnit (`0x23`) | `0x4056D1` | A of the target | `0x41BD10` at `0x40561A` | tick |
| `0x405740` | RepairUnitNoMove (`0x24`) | `0x4058F6` | A | `0x41BD10` at `0x40583F` | tick |
| `0x414E70` | VTOL_RepairUnit (`0x3D`) | `0x4151EC` | A | state 2 with event bit `0x40` clear, the target's `+0x110 & 3` = 1 and `& 0xC` = 0, and its HP below the def's `+0x1FA`; `0x41BD10`'s result at `0x41513D` is never tested | tick |
| `0x404DB0` | Resurrect (`0x25`) | `0x40509D` | F | state 4, the feature still present, the countdown `order+0x3A` nonzero before its decrement | tick |
| `0x404270` | Capture (`0x0E`) | `0x404676` | A of the target | state 4, `order+0x36` below `order+0x3A` | 2 ticks |
| `0x404730` | ReclaimUnit (`0x21`) | `0x404A4E` | A | in range, and `0x489960` at `0x404961` | 2 ticks |
| `0x414A80` | VTOL_ReclaimUnit (`0x3B`) | `0x414C43` | A | in range, and `0x489960` at `0x414B6D` | 2 ticks |
| `0x404AD0` | Reclaim (`0x20`) | `0x404D3B` and `0x404D4C`, the same arguments | F | the feature present, the countdown `order+0x36` above 15 after −2 | 2 ticks |
| `0x414770` | VTOL_Reclaim (`0x3A`) | `0x414A20` and `0x414A31` | F | the same, above 30 | 2 ticks |

The first ten sites call the forward emitter, the last seven the reverse one. With P the
target's position `+0x6A/+0x6E/+0x72` and D its def's dwords `+0x15E..+0x172`, box A is
`{Px+D15E, Py, Pz+D166, Px+D16A, Py+D16E, Pz+D172}`, B is A with the lower y `Py+D162`, and F is
the feature's: the grid words `0x421DA0` wrote, shifted by 20, the feature def's footprint
`+0x94`/`+0x96` and height `+0xFA`, and the ground height `0x485070`. `0x43E400` is called 15
times for the 17 sites: each Reclaim pair shares one call (`0x404CC4`, `0x4149A9`).

- **Where a handler can emit.** Handlers are called at exactly three places, each with
  `(order+0xE, order, events)` and the raw type byte as the index: `0x43B87C` in the main-list
  controller `0x43B7C0`, which runs the head of `unit+0x5C`, re-reads it after every call and
  passes the fired events `(order+0x4E | unit+0xBA) & order+6`; `0x43BB21` in `0x43BAD0`, which
  walks the sub list `+0x60` and passes 0; and `0x43A21A` in the order node's destructor
  `0x43A1F0`, which passes 2. The two controllers have one caller each, `0x48AF98` and `0x48AF9E`
  in the unit tick, behind the gate that the unit's owner record be active and of type 1 or 2
  (`0x48AEB5..0x48AECD`). None of the fifteen records has flag `0x40000`, so these orders always
  live on the main list and only `0x43B87C` runs them: **an emission happens only in the unit
  tick on the game thread, for a unit whose owner is a local seat (type 1 or 2).**
- **The destructor cannot emit.** `0x43A1F0` has twelve callers (`0x4384E4`, `0x439EF3`,
  `0x439F5F`, `0x439FC9`, `0x43AE47`, `0x43AEB8`, `0x43B075`, `0x43B1C9`, `0x43B93B`, `0x43BA5B`,
  `0x43BBE6`, `0x43BC49`) and no vtable entry; it calls the handler only when `order+6` has bit
  `0x02` (`0x43A1F5..0x43A200`). Nine handler functions set it, and these are all their stores of
  `+0x06`: SelfDestruct/SelfDestructFG `0x402010` (`0x402104`, `0x40212B`), BuildingBuild
  (`0x4028B5`, `0x402922`, `0x402AE6`, and `0x438700(…, 2)` at `0x4027CA`), MobileBuild (`0x403F0C`,
  and `0x438700(…, 0xA)` at `0x403DFD`), HelpBuild (`0x404202`, `0x404239`, and `0x438700(…, 0xA)`
  at `0x4040F6`), AirStrike `0x411F50` (`0x412293`, `0x412526`, `0x4125BB`), AirToGround `0x412710`
  (`0x412CE3`), VTOL_Pickup `0x4111B0` (`0x411464`), VTOL_MobileBuild (`0x4142DF`, and
  `0x438700(…, 0xA)` at `0x41413E`) and VTOL_HelpBuild (`0x414700`). `0x438700` stores its mask
  `| 4` when the unit's `+0x10F` bit 0 is clear (`0x438711..0x43871C`), `0x438730` when bit 1 is
  set (`0x438741..0x43874C`). Of the fifteen, then, the five build handlers set the bit, and each
  returns on event 2 before its emitter; the other ten would reach their emitter on event 2, and
  never set it.
- **What the destructor does instead.** BuildingBuild's event-2 cleanup (`0x402653..0x402723`)
  credits `unit+0xD4`, calls `0x41B8D0(builder, nanoframe)` and deals the unfinished nanoframe
  `0x489BB0(builder, nanoframe, 30000, 9, 0)` (`0x402701`), then `0x48B090` and `0x41C150`; the
  other four build handlers call `0x41C110`. The destructor itself starts COB `StopBuilding` on
  the owner when `order+0x42` has bit `0x400000` (`0x43A21E..0x43A25D`; `0x438590` sets the bit
  after starting `StartBuilding`, `0x4385F0` clears it) and broadcasts it (`0x43A258`). All of
  this runs wherever the destructor runs: unit death, player commands, the AI's orders, an attach,
  the level teardown, and `0x43AFC0` with shift 0, which reaches `0x43ADC0` with queue 0 and
  destroys the unit's main-list nodes without flag 4 (`0x43AE0A..0x43AE59`). So a new order given
  to a factory mid-build kills its nanoframe.
- **Handler calls nest.** BuildingBuild, MobileBuild and VTOL_MobileBuild send `getbuilt` to the
  new nanoframe through `0x43ADC0` (`0x4029A5`, `0x403DC5`, `0x4140F3`), which destroys that
  unit's leading nodes with flag `0x4000`; an attach, `0x48AAC0` → `0x48AB70` → `0x4384A0`, clears
  the child's main list. Attribution by a single "current unit" set around `0x43B87C` would be
  wrong; each call's own first argument, kept as a stack, is not.
- **Cadence.** `0x43B7C0` runs once per unit per tick. The build and repair states, SelfRepair
  and Resurrect re-arm with `0x439E80(order, 1)`, capture and the reclaims with
  `0x439E80(order, 2)`. At a factory's unit turnover the states chain inside one controller call
  (state 3 returns 1, state 4 returns 0, state 0 with units left returns 1, state 2 creates the
  next nanoframe and returns 1), so two emissions in one tick are possible [the chain DIS; how
  often INFERRED]. VTOL_MobileBuild's state 3 falls into state 4's build code (`0x414143 jmp
  0x414149`), so a VTOL builder steps and sprays from both.
- **The steps.** `0x41BA60(builder, target, work)` returns 1 only for work > 0 when the request
  `0x4011C0` on the builder's ledger `+0xBC` succeeds; it then raises the target's HP `+0x108`
  toward the def's `+0x1FA`, lowers its remaining fraction `+0x104` and sets `+0x110` bit
  `0x2000`. It returns 0 when `+0x104` is already 0.0, when work is 0, when the request fails
  (nothing applied), and on the unbuild path (work < 0, from `0x41BCD0`, which GetBuilt calls at
  `0x402F6B`), which applies progress in reverse. `0x41BD10(repairer, target, work)` returns 0
  at or above the target def's max HP (`0x41BD33`); otherwise it caps both the HP to add and the
  energy to ask at 1 a call (`0x41BD87..0x41BDA3`), asks `0x401180` on the repairer's ledger and,
  when that succeeds, applies `0x489BB0(repairer, target, hp, 0xA, 0)` and returns 1. `0x489960`
  (thiscall, `ret 4`, writes nothing) is an eligibility test, not a step: the reclaimer's def
  `+0x245` bit `0x400`, the target's `+0x110 & 3` ≠ 2 and its def's `+0x245` bit `0x1000` clear
  (`0x489960..0x48999E`). ReclaimUnit's progress is a kind-5 hit `0x489BB0` once `order+0x3A`
  reaches 15 (it grows by 2 a call), and it does not gate the spray.
- **A reclaimed unit is never a nanoframe** [DISASSEMBLED 2026-10-27; MEASURED the same day, below].
  ReclaimUnit `0x404730` sends `0x489BB0(reclaimer, order+0x16, order+0x36, 5, 0)`
  (`0x404974..0x404981`) and resets `order+0x3A` (`0x404986`); VTOL_ReclaimUnit `0x414A80` sends
  the same hit at `0x414B8D`. The sender `0x489BB0` calls the apply `0x489CE0` (`0x489C89`, or sends
  the record), which lowers the target's HP `+0x108`. No instruction of `0x404730..0x404ACF`,
  `0x414A80..0x414C7B`, `0x489BB0..0x489CDF` or `0x489CE0..0x489F7F` names `+0x104`, so the target's
  build fraction stays 0.0 and it draws as a finished unit until it dies. The one unbuild,
  `0x41BCD0(unit, n)` (`ret 8`: `0x41BA60(unit, unit, −(def +0x1EA · n) / def +0x186)`, the unit its
  own builder), is not the reclaim: its only direct caller is GetBuilt's `0x402F6B`, after
  `0x439E80(order, 0xB)`. MEASURED in a skirmish (`tools/wiki_regression.py units`, step u7): an
  ARMCK ordered to reclaim a finished ARMSOLAR took its HP to 0 while `+0x104` read 0.0 at every
  read. The in-game wiki's stage draws a reclaim so (`tagpu_wikiscene.c`).
- **The ledger** `unit+0xBC` is 0x34 bytes: an energy group at `+0` and a metal group at `+0x18`,
  each six floats {produced, requested, granted, debt, last produced, last requested}, and the
  owner's slot in the player array at `+0x30` (`0x401070(owner index)`, which zeroes the 0x34
  bytes and sets `+0x30`; called by the unit's creation `0x485D0A` and for the player's own
  ledger, the `+0xEC` record, at `0x4647EA`) [DISASSEMBLED 2026-10-10]. In the unit record:

  | unit | ledger | what it holds |
  |---|---|---|
  | `+0xBC` / `+0xD4` | `+0x00` / `+0x18` | energy / metal produced since the last economy pass |
  | `+0xC0` / `+0xD8` | `+0x04` / `+0x1C` | energy / metal requested since the last pass, granted or not |
  | `+0xC4` / `+0xDC` | `+0x08` / `+0x20` | energy / metal granted since the last pass, paid at the next |
  | `+0xC8` / `+0xE0` | `+0x0C` / `+0x24` | energy / metal debt: granted at an earlier pass and not paid |
  | `+0xCC` / `+0xE4` | `+0x10` / `+0x28` | energy / metal produced in the last pass's period |
  | `+0xD0` / `+0xE8` | `+0x14` / `+0x2C` | energy / metal requested in the last pass's period |

  **The writers.** `0x4011C0(e, m)` (thiscall on the ledger, `ret 8`) always adds the request to
  `+0x04`/`+0x1C`, returns 0 while either debt `+0x0C`/`+0x24` is above 0.0, and otherwise adds it
  to the granted `+0x08`/`+0x20` and returns 1; it never reads stored resources. Its callers are
  the build step `0x41BA60` (`0x41BC60`) and BuildWeapon `0x402B70`, the stockpile
  (`0x402C6B`). `0x401180(e)` is its energy-only twin, called by the repair step `0x41BD10`
  (`0x41BDB7`). `0x4012A0(e, m)` pays at once from the owner's stored energy `+0x8C` and metal
  `+0x98` when both suffice, adds to the requested `+0x04`/`+0x1C` only and returns 1, else 0
  with nothing written: a weapon's shot, from its def's `+0xC0`/`+0xC4` (`0x49E533`, the fire
  path, skipped when the shooter's `+0x111` bit 28 is set). `0x401220`/`0x401260` are the same
  for one resource and only ever run on a player's `+0xEC` record (`0x464BC4`, `0x464CF4`). The
  economy pass `0x401360` itself, once a pass and behind each unit's state bits, adds the def's
  `+0x1C6` when positive to the energy requested `+0xC0` and, with no energy debt, to the granted
  `+0xC4` (`0x4013ED..0x401421`, `0x401643..0x401668`), and when negative its negation to the
  produced `+0xBC`; the metal a unit makes into `+0xD4`, only when that energy was granted
  (`0x401480..0x401545`); two def factors times two `main` values into `+0xBC` (`+0x1D2` ×
  `main+0x37EDE`, `+0x1D6` × `main+0x14267`, `0x401550..0x4016B5`) [INFERRED: tidal and wind];
  a finished unit's def `+0x1C2` into `+0xBC` and `+0x1CA` into `+0xD4` (`0x4016DC..0x401795`);
  and a cloaked unit's cost (def `+0x1DA`, or `+0x1DE` when `+0x110 & 0xC`) paid from storage
  into `+0xC0` (`0x401806..0x40185E`). A type-2 player's production is scaled by the difficulty
  in each of these. Nothing else in the exe stores to these unit offsets (every
  float and dword store to `+0xC0`, `+0xC4`, `+0xD8`, `+0xDC` searched: the rest are weapon defs
  `0x42E56D..0x42E5AE`, the player loader `0x4661BD` and other records).

  **The settle.** The economy pass runs for each type-1/2 player on its 30-tick pass (`0x46555A`,
  *The Deathmatch respawn*). It sums the six current floats of every unit with `+0x110` bit 28 and
  of the `+0xEC` record (`0x401877..0x40193D`), pays the debts first and then the granted from
  current plus production, keeping the fraction paid of each (`0x401A4D..0x401AB1`), and then
  settles every such unit (`0x401B3D..0x401B99`): produced → last produced, requested → last
  requested, the three zeroed, and debt := granted × (1 − the fraction of granted paid) + debt ×
  (1 − the fraction of debts paid). So a stalled owner's builders fail every request until a
  later pass pays. The player's expense `+0x94`/`+0xA0` is the summed **requested**, refused
  requests included (`0x4019DD`, `0x401A05`).

  **What it costs a recording.** `+0xC0`, `+0xC4`, `+0xD8` and `+0xDC` are float accumulators
  that grow every tick a unit builds, repairs, stockpiles or fires a weapon that costs a resource,
  and fall to 0 at its owner's pass. On the heavy four-AI hour they are four of the unit frame's
  thirteen most changed dwords, about 18.6 million changes between them, each typically 21–25 bits
  of the float's bits ([the size note](../tadr-port/demo-recorder-size.md#l4-the-measured-hours-the-snapshot-interval-and-r1)).
  For the eight step-gated sites, "sprayed this tick" is therefore "the step returned 1 this
  tick"; VTOL_RepairUnit, capture, the reclaims and resurrect spray with no resource condition
  while their state gates hold.
- **Other writes around the spray.** The handlers stamp the unit's `+0xB0` with GameTime + 150
  (the repairs: `0x4024E1`, `0x4055E9`, `0x40580E`), + 300 (MobileBuild `0x403EE2`, HelpBuild
  `0x4041D8`, Reclaim `0x404CAC`, Resurrect `0x4050B7`, VTOL_Reclaim `0x414991`) or + 900 (Capture
  `0x404692`, ReclaimUnit `0x4049A3`); the shot path `0x49C740` stamps + 600. Its one reader found
  is the economy pass's cloak cost (`0x4017F8`, *The unit and player records, byte by byte*).
- **`0x43E400(unit, out)`** (stdcall, `ret 8`) runs COB `QueryNanoPiece` (`0x5052D4`) through
  `0x4B0BC0` on `unit+0x9A` (`0x43E427`) with the piece preset to 0, turns the piece into a
  model-space point with `0x43DEF0` (`0x43E437`) and adds the unit's `+0x6A/+0x6E/+0x72`. It writes
  only `out` (`0x43E458..0x43E45D`) and the query's own COB thread record. With no script, all
  eight thread records busy, a null `+0x9E` or a piece out of range, the point is the unit's
  origin. The query runs where the handler runs, on the owner's machine; a recording carries it
  and a playing makes it again (*The owner's COB queries*).

### The nano particle: its boxes, its two bursts, its step, its dot [DISASSEMBLED 2026-10-28]

What the in-game wiki's spray repeats (`tagpu_wikisim.c` *THE NANO SPRAY*, [the COB
plan](../ingame-wiki-cob-plan.html), *The spray*). DISASSEMBLED on `pristine/TotalA.exe.pristine`
(`objdump -d -M intel`); caller counts are a scan of `.text` for direct `call`/`jmp` to the
target. The particle object is `effects.md` §7.2's; this is its nano class, vtable `0x4FD5B8`,
whose seven slots read from `.rdata` are `0x471560` destroy, `0x472EB0` update, `0x472F90` draw,
`0x472FD0` empty, `0x473D50` spawn, `0x472F60` spawn-again and `0x473B50` init.
`0x473B50` and `0x473D50` have no direct caller: they are reached through the vtable only.

- **Where an emission starts.** MobileBuild steps at `0x403E43` and, only when `0x41BA60`
  returned nonzero (`0x403E4A je 0x403ED1` skips both), asks `0x43E400` at `0x403E56`, builds box
  B (`0x403E5B..0x403EC8`) and emits at `0x403ECC`. ReclaimUnit `0x404730` sprays from its state 5
  (the jump table `0x404AB0`, entry 5 = `0x4048D2`): in range (`0x404953`) and eligible
  (`0x489960` at `0x404961`), it may send its hit (`0x404974..0x404986`), stamps `+0xB0`, asks
  `0x43E400` at `0x4049AD`, builds box A of the target (`0x4049B2..0x404A4A`), emits in reverse at
  `0x404A4E` and re-arms itself two ticks on (`0x439E80(order, 2)` at `0x404A57`). Out of range or
  ineligible it calls `0x439E80(order, 15)` and `0x4385F0` and returns 0 with no spray
  (`0x404A74..0x404A8D`). So a reclaim sprays every second tick from its first spraying call.
- **The target box.** The def loader writes D15E = −(FootprintX `def+0x14A` << 20)/2, D166 the
  same of FootprintZ `+0x14C`, D16A and D172 their positives (`0x42D079..0x42D0C5`, each `sar 1`
  after the sign fix: ±FootprintX × 8 world units in 16.16), D162 = 0 (`0x42D7A5`) and D16E =
  `0x4CB5F0(model)` (`0x42D7BF`, stored at `0x42D7CA`). So box A and box B are equal in stock: the
  target's footprint by the ground to its model's top.
- **`0x4CB5F0(node)`, the model's top** (stdcall, `ret 4`; called at `0x42D7BF` and by itself at
  `0x4CB627`): over the sibling chain from `node` (`+0x2C`), the most of 0 and, for each node, each
  vertex's y (`+0x24` the array, 12 bytes a vertex, y at `+4`; `+0x04` the count) plus the node's y
  offset `+0x14`, and its child chain's (`+0x30`) top plus the same offset, with plain 32-bit adds
  (`0x4CB5F0..0x4CB641`). The wiki's 3D load and `gfx.py` compute it the same way
  (`tagpu_wikiasset.c` `model_top`, `gfx.py` `model_shape`).
- **The init `0x473B50(box1, box2, life)`** (thiscall): end tick `+0x04` = GameTime + life through
  `0x471D70` (`0x473B5B`; both emitters pass life 1, `0x47216F` and `0x4722A3`); box1 to
  `+0x1C..+0x30` (`0x473B6E`), box2 to `+0x34..+0x48` (`0x473B7E`); then **each box cut, per
  axis, to its middle**: with d = max − min, the low edge becomes min + 4·d/11 and the high min +
  7·d/11, each product a 32-bit `lea`/`shl` and the division by 11 the multiply by `0x2E8BA2E9`
  then `sar 1` with the sign fix, so toward zero (`0x473B80..0x473C0C` for box2's x and y, the
  same through `0x473D34`); then each high edge is replaced by its extent, high − low. The box is
  kept as {base x, y, z, extent x, y, z}; the spawn follows (`call [vt+0x10]` at `0x473D39`). A
  degenerate box (the forward emitter's point) keeps base = the point and extent 0.
- **The spawn `0x473D50`**: five particles a call (`[esp+0x18] = 5` at `0x473DA5`, the loop back
  at `0x474109`); for each, six draws of the CRT `rand 0x4E4870` in the order start x, y, z (box1)
  and end x, y, z (box2), each `base + rand·extent / 0x8000`, the product 64-bit (`imul`) and the
  division `_alldiv 0x4E4440`, toward zero (`0x473DAD..0x473E54`). The six draws are made whatever
  the extents. Then d = end − start, len = `_ftol` (`0x4E43A0`) of the x87 `fsqrt` of the sum of
  the squares (`0x473E6D..0x473EB1`), and **steps = the high word, as int16, of (len << 16) /
  0x40000** (`_allshl 0x4E44F0` by 16, `_alldiv` by `0x40000`, the word at `[esp+0x16]`;
  `0x473EB6..0x473ED9`), which for len ≥ 0 is `(int16)(len >> 18)`: a quarter of the length in
  world units. **A particle of 0 steps is not made** (`0x473EDE..0x473EE1 je 0x4740F7`), its draws
  spent. The step is d / steps per axis by `idiv`, toward zero (`0x473EE7..0x473F04`); the end
  tick `+0x2C` = GameTime + steps (`0x473F08..0x473F1E`); `+0x24` = `0x100`; the colour `+0x28` =
  `0xA1 + i % 7`, i the particle's place in the call, 0..4 (`0x473F13..0x473F41`). After the loop
  the last-spawn tick `+0x08` = GameTime + 1 (`0x47410F..0x47411E`).
- **The update `0x472EB0`** (slot 1): for each particle the move `0x4739B0` then the death test
  `0x473B30(GameTime)`, which answers `GameTime > end` (`setg`, `0x473B3A..0x473B3D`); a dead one is
  removed by moving the rest down (`0x472EEC..0x472F1F`). Then the spawn-again test `0x472F60`
  (`+0x08 <= +0x04`, signed, and `+0x08 <= GameTime`, unsigned) calls slot 4 again
  (`0x472F37..0x472F48`).
- **The move `0x4739B0`**: x, altitude, z += the step (`+0x18/+0x1C/+0x20`); the colour's low
  nibble + 1, and past 7 back to 1, over `colour & 0xFFF0` (`0x4739B6..0x4739F1`), so a particle
  cycles `0xA1..0xA7`. No gravity, no drag. One caller, `0x472ED0`.
- **The walker `0x471EB0`** (one caller, `0x4955BF` in the game loop step `0x495490`, after the unit
  tick `0x48AD30` at `0x4954ED`; GameTime `main+0x38A47` is advanced at `0x4954C0`, before both):
  for each of the ten layers, for each object, slot 3 first (`0x471EE0`); an object whose
  sub-vector is empty (`0x472FD0`) is destroyed and the rest shifted down (`0x471EE7..0x471F16`),
  any other updated through slot 1 (`0x471F1C`).
- **Two bursts an emission.** At tick T the handler's emitter makes burst 1 in the unit's turn; the
  walker of the same tick moves it (the update) and the spawn-again test fails (`+0x08` = T + 1 >
  T). At T + 1 the object is not empty (a particle of burst 1 has steps ≥ 1 and so dies at T +
  steps + 1 at the earliest), its particles move, and the test passes: burst 2 is made **after**
  that tick's moves, and sets `+0x08` = T + 2 > `+0x04` = T + 1, so there is no third. An emission
  whose burst 1 made no particle is empty at T's walker and destroyed: no burst 2. A particle of
  burst 1 moves at T … T + steps and dies at T + steps + 1, one step past its end point; one of
  burst 2 moves from T + 2. [The gates DISASSEMBLED; the T / T + 1 order follows from the call
  order of `0x495490`, not measured live.]
- **The draw `0x473A00(ctx, eyeX, eyeY)`** (thiscall on the particle, `ret 0xC`; one caller,
  `0x472FBB` in slot 2 `0x472F90`, with the words `main+0x1431F` and `main+0x14323`): sx =
  (int16)(hi(x) − eyeX) + `0x80`, sy = (int16)(hi(z) − eyeY) − (hi(alt) >> 1) + `0x20`, the shift
  arithmetic on the sign-extended word (`0x473A10..0x473A43`); the rect {sx, sy, sx + 1, sy + 1}.
  The sight test is the viewed player's (`main+0x2A43`, record `main+0x1B63 + 0x14B·p`) at the tile
  (hi(x) >> 5, (hi(z) − (hi(alt) >> 1)) >> 5): with `main+0x14281` bit 1 set its byte map at record
  `+0x7C` (width `+0x80`, height `+0x84`) nonzero, else the mapped word map `*(main+0x14273)`'s bit
  p; off the map, not drawn (`0x473A50..0x473B05`). Then `DrawBar 0x4BF6F0(ctx, &rect, colour)`
  at `0x473B1D`.
- **`DrawBar` fills 2 × 2 here.** `0x4BF6F0` copies the rect, clips it with `0x4BF620` and fills
  through `0x4CCDEA` (`0x4BF754`, or `0x4BF790` with a context given); `0x4CCDEA` writes
  right − left + 1 columns (`0x4CCE1B..0x4CCE20`, the byte path `0x4CCE61..0x4CCE66`) and
  bottom − top + 1 rows (the count `0x4CCE25..0x4CCE2B`, looped while `dec esi` stays
  non-negative, `0x4CCE42`, `0x4CCE7C`): {sx, sy, sx + 1, sy + 1} is two pixels by two, in the
  colour's low byte.
- **The piece origin `0x43DEF0`** (*The piece transform*, below, has the whole rule): a piece
  `< 0` or `>= [o3]` answers (0, 0, 0) (`0x43DF16`, `0x43DF1E` → `0x43E01D`); each ancestor's angle
  triple is read and, at the root (no parent, `0x43DF73`), the unit's `+0x64`, `+0x68`, `+0x66` are
  added to its z, x and y words (`0x43DF86..0x43DF9C`) before `0x4B6CC0` turns what is below it
  (`0x43DFB0`); the root's own offset and position are then added unturned; z is negated at
  `0x43E00A`. `0x4B7173` returns at `0x4B717B` for a zero angle and stores both results with
  `fistp` in the FPU's current rounding mode, to the nearest by default (`0x4B71A0`, `0x4B71A2`).
- **What the wiki repeats and where it differs** (`tagpu_wikisim.c`, `sim.js`): the boxes, the
  cut, the five-a-burst spawn, the steps, the 0-step rule, the step, the end, the colour and its
  cycle, the two bursts and their order, and the reclaim's two-tick cadence, all in int32 as the
  engine's. It draws from its own generator, never `0x4E4870` (decision 11 of the COB plan); its
  sight test is its own fog grid's 16-unit cell, grey or black hidden, not the engine's 32-unit
  tile; its colours are the retail palette's (below).
- **The colours** `0xA1..0xA7` in the retail `palettes\PALETTE.PAL`, MEASURED 2026-10-28: (171,
  231, 127), (131, 211, 91), (103, 191, 63), (75, 171, 43), (67, 151, 43), (55, 135, 39), (47, 119,
  27), a light-to-dark green ramp. Whether the palette in play equals the file at those indices was
  not checked.

### The demo recorder's pool frame: the engine facts it predicts from [DISASSEMBLED + MEASURED 2026-10-08]

Recording format 25 carries a stream block's projectile records as the *pool frame*
(`tagpu/ddraw/src/tagpu_reccodec.c`, THE POOL FRAME; the bytes in the skirmish slice's
[*The file*](../tadr-port/demo-recorder-skirmish-slice.md#the-file), the measurements in
[making recordings smaller](../tadr-port/demo-recorder-size.md#l2-the-pool-frame-measured)): the
pool coded by identity, each surviving record predicted from its own last image, each new one
against a reference row. Every fact below is a predictor's: a wrong one costs size, never data,
because the writer writes a block in the frame only when a second decoder gives its records back.
Each was re-read in `pristine/TotalA.exe.pristine` on 2026-10-08 (`objdump -d -M intel`); the
pool's layout and its appends are *The projectile pool* above.

- **Survivors keep their order and come first.** The compaction `0x49AE20` walks `[0, count)`,
  tests the done bit `+0x69` bit 1 (`shr cl,1; test cl,1` at `0x49AE63..0x49AE65`) and copies each
  survivor behind the first hole whole; every append is at the count. So an iteration is the old
  records that died, the survivors in order at `[0, c0 − died)`, the births after them, and the
  stale slots past the new count, which keep their bytes.
- **The stamp `+0x67` is the old slot.** The compaction stores each record's index before the move
  in the word at `+0x67` (`mov word [eax+0x11], si` at `0x49AE5F`, `eax` = the record + `0x56`, so
  `+0x13` from it is `+0x69` at `0x49AE5C`), the removed records included. The frame predicts a
  survivor's stamp as its old slot and a birth's as `c0 + i`, the slot it was appended at. From
  format 22 a survivor's bytes `0x34..0x6A` are coded as lanes at the record's field widths, the
  stamp a 2-byte lane of its own (`+0x67..+0x68`): the fields from `+0x3A` on sit at 2 mod 4
  (`+0x4E` the target, `+0x52` the attacker, `+0x56` the quarry; `replay-layout.tsv`'s `proj`
  rows), so the dwords there each straddle two fields. The widths are a coding choice: a wrong
  one costs size.
- **The guided velocity** (the projectile tick, the branch for a weapon with `+0x111` bit 20,
  tested at `0x49B9C2..0x49B9D0`). A record whose death GameTime `+0x46` is still ahead of
  GameTime `main+0x38A47` (`cmp` at `0x49B9DF`, `jbe` to the expired path `0x49BAC3`, which for
  bit 23 calls `0x499EB0` and otherwise subtracts gravity `main+0x14263` from vy at
  `0x49BAD3..0x49BADE`) takes the
  speed step — `+0x3A += weapon +0x70` while below `weapon +0x68`, then capped at it
  (`0x49BA16..0x49BA31`, unsigned compares) — and the aim and turn (`0x49BA34..0x49BA6F`, through
  `0x49B3E0`, `0x49B520` and `0x499EB0`), then sets the velocity from the angles and the speed
  (`0x49BA74..0x49BABE`): vy `+0x20` = `0x4B70EF`(pitch `+0x38`, speed `+0x3A`), h =
  `0x4B7123`(pitch, speed), vx `+0x1C` = −`0x4B70EF`(heading `+0x36`, h), vz `+0x24` =
  −`0x4B7123`(heading, h). With bit 16 also set and a word compare against the sea level
  `main+0x1427F` passing (`0x49B9EB..0x49BA11`), it subtracts gravity from vy and stores the pitch
  instead [the compared word not traced]. The codec has no weapon table, so it does not test bit
  20: it applies the rule to a survivor whose last image already satisfies it (`held`, a pure
  function of the image), and to a birth whose reference row does.
- **Position += velocity** for every record that survives the tick, after the velocity's update:
  `+0x04 += +0x1C`, `+0x08 += +0x20`, `+0x0C += +0x24`, each branch with its own add (the guided
  one `0x49BB3A..0x49BB58`; `0x49BB82`, `0x49BC37`, `0x49BCE3`, `0x49BD12`, `0x49BD50`); the paths
  without one (`0x49BC67` → `0x499EB0`, and `0x49BCD0`) mark the record done (`0x499EB0` sets
  `+0x69` bit 1 at `0x499F32` and `0x499FA0`). The frame predicts the
  position from the velocity it has just decoded, plus the last step's drift (whatever moved the
  record besides its velocity).
- **`0x4B70EF` and `0x4B7123`** are the sine and cosine products of *`DrawRangeCircle
  0x438EA0`*, below (the table `0x509F00`, `+0x1000` and `shrd 13`), equal to the codec's `sinmul` and
  `cosmul` on every input: bits 7–15 of `angle + 0x20` do not depend on how the word is extended.
- **The shot packet `0x0D`** (built at `0x49D7AD..0x49D859` and sent through `0x451DF0`, 36
  bytes): `+0x01..+0x0C` three dwords of the fire function's locals (`[esp+0x18..0x20]`, the start
  its build used, which `0x49C740` copies to the record's position and tail; `0x49D9C0` alone sends
  the shooter's centre there, and the recorder writes its build's start in the event instead:
  [*What a shot's packet says of its start*](recorder.md#what-a-shots-packet-says-of-its-start-0x49d9c0-0x49d270-disassembled-measured-2026-10-09)), `+0x0D..+0x18` the three dwords at `[ebp]` (the target point),
  `+0x19` the weapon's ID byte (`[weapon+0x10A]`, `0x49D7E5`), `+0x1F` the target's and `+0x21`
  the shooter's unit index (`[unit+0xA8]`, `0x49D812` and `0x49D802`), `+0x23` the weapon slot
  (`shr al,2; and al,3`, `0x49D7D9..0x49D7ED`), where the raised build also writes the weapon
  ID's bits 8–11 in the high nibble (`tagpu_patches.c`, THE WIRE). The recorder keeps it as
  `RK_EV_SHOT`; the frame makes a reference row of each of the last iteration's shots from the
  last birth of that weapon (`+0x00` = the twelve-bit ID + 1), its position and tail from the start, its target point and its two unit IDs. In the
  stock-limits build `+0x23`'s high nibble is not weapon bits [INFERRED], so a row may name the
  wrong weapon: size only.

**What each fact buys**, MEASURED by the format's prototype on L1's four recordings (the battle,
Town & Country, naval, the air war; the pool frame packed alone, zstd 3, MiB, on a configuration
slightly before the final one): without the stamp 2.73 / 1.24 / 0.43 / 0.50 against 2.42 / 1.11 /
0.40 / 0.46; without the shot events 2.98 / 1.30 / 0.44 / 0.51. The guided rule saves 0.31 MiB on
Town & Country and 0.14 on naval and on the air war, nothing on the battle, where applied to every
survivor it *cost* 0.20 MiB; the `held` test is what removes that cost. The rule on births holds
for 63 % of Town & Country's births and 53 % of the air war's, −0.13 MiB on the battle.

**Not predicted**, each costing size only: a birth's fields its builder computes from the
attacker (a burst copy's spray, the RNG), the guidance's turn (`0x49B3E0` reads the target's
position), the bit-16 branch and the expired path's gravity, and a record's last cell hit
`+0x5A..+0x5F` (the collision `0x49B090`) — the survivors' dword 23, `+0x5C..+0x5F`, is the
largest residual column of the battle, 214 KiB of its 2.09 MiB.

### A weapon's ID, and the messages that carry it — `0x42E440`, `0x49D270`, `0x45544D`, `0x42454B` [DISASSEMBLED + MEASURED 2026-09-24]

**The loader writes wherever the ID points.** `LoadWeaponTdf 0x42E440` reads `ID` with a default of
−1 (`0x42E463`) and takes the record at `main + id·0x115 + 0x2CF3` (`0x42E468..0x42E489`) with no
bound either way. `Weapons[256]` ends at `main+0x141F3`, the projectile pool's count and pointer, so
ID 256 overwrites the pool's header; ID −1 overwrites `main+0x2BDE..0x2CF2`, the UI and input block
(the mouse position, the hovered cell, the build footprint, the mode byte `0x2CC3`, `BuildUnitID`
`0x2CC4`). Stock content's highest ID is 246, and every section has one
([evidence §9](../tadr-port/limits-evidence.md)).

**The weapon-fired receiver `0x49D270` (`0x0D`) takes two unit indexes and a slot from the wire
unbounded.** It indexes `Weapons[+0x19]` (`0x49D27B..0x49D29F`), and for a weapon without flag bit 5
(`0x49D2A6`) scales the `u16` shooter at `+0x21` (`0x49D329..0x49D34A`) and the `u16` target at
`+0x1F` (`0x49D388..0x49D3B7`) by 0x118 into the unit array `main+0x14357`, then reads through both;
the slot byte `+0x23` becomes the shooter's slot at `+4 + slot·0x1C` (`0x49D366..0x49D370`), which
it writes (`0x49D37C`, `0x49D384`).

**The feature-hit receiver `0x45544D` (`0x0F`) reads a weapon's ID byte as a sentinel.** The message is
the type, a byte, and the cell's `u16` x and y. The bytes `0xFD`, `0xFE` and `0xFF` mean "destroyed"
(`FeatureDie(x, y, 0)`, `0x423550`), "burned" (`0x4233A0(x, y, 1)`) and "reclaimed" (`FeatureDie(x, y,
1)`); anything else is `Weapons[byte]` and goes to `0x4244B0`. So a weapon with ID 253–255 that hits a
feature destroys, burns or reclaims it on every other peer, where the peer that fired it damaged it.
The cell comes from `0x481550`, which returns NULL off the map, and `0x4244B0` reads through it at
`0x4244CF`.

**Who sends a feature hit.** Area damage `0x49A120` calls `0x4244B0(cell, x, y, weapon)` for every
cell of its radius that holds a feature (`0x49A626`). `0x4244B0` acts only while `main+0x37F2F` has
bit 3 (`0x4244C2`; it holds `0x041C` in play), on an anchor cell (`+0x08` below `0xFFFB`) of a
feature whose def lacks bit 9 of `+0xFE`. In a network game (`[main+0x391E9]` read through
`0x435100` is 3 [INFERRED role]) a peer whose local player (`main+0x2A43`) has bit 0 at `+0x97` of
the object `[main+0x1B8A + 0x14B·player]` damages the feature; every other peer sends the hit
instead: `0x42454B` builds the `0x0F` and `0x424575` sends it, `0x451BC0` with four arguments (the
first `0x44FDB0`'s, a scan of the ten player records; the message third; `ret 0x10`). MEASURED: the
host damages, the joiner sends. The sentinels are broadcast through `0x451DF0` by whichever peer
runs the outcome, and none of their senders tests the host bit: `0xFD` from `0x42469A`; `0xFE`
from `0x423537`, whenever `0x4233A0` runs with its third argument 0 (callers `0x423A4D`,
`0x423B55`, `0x4245C6`, `0x4250BB`); `0xFF` from `0x4239A4`, on the peer of the reclaiming unit's
owner, and from `0x405210` in `0x404DB0` [INFERRED: the resurrect order]. A cell is a 16.16
position shifted right by 20 (`0x4815A0`), so no reachable x passes `0x7FF`.

**The fixes, both builds** (`fix_weapon_ids`; rows of the fail-closed table in both builds, and in
the raised build the raise's own rows, *Weapon IDs* in *The raised effect pools*):

- `0x42E468`: a weapon whose ID is outside the array is skipped and logged, through the loader's
  epilogue `0x42F333`, past its last call `0x49E010(weapon)`, which sets the record's `+0x60` from
  its flags at `+0x111` [role INFERRED: the projectile's handler]; nothing of the record is
  written. So is a weapon whose section name does not fit its 0x115-byte record: `0x42E490` copies
  it to `+0` with `repnz scas` / `rep movs` and no bound, and the TDF parser cuts a section name
  out of the file's text (`0x4C4340`, trimmed of blanks), so nothing bounds its length. A name of
  0x20 or more already runs into the description the loader writes next at `+0x20` (`0x4C48C0`,
  at most 0x40), as in stock. A duplicate ID keeps stock's rule, the later wins, and is logged.
- `0x49D280`, 16 bytes: the weapon from the full ID, and the shooter and target each bounded by the
  unit array's last element (`main+0x1435B`, inclusive: `0x4855D6` sets it to begin + (count −
  1)·0x118, and the engine's own sweep `0x48BD00` steps with `add eax,0x118` at `0x48BD22` and
  loops `jbe` to it at `0x48BD38`); a message past it is dropped through `0x49D55D`. The slot byte `+0x23`, which
  `0x49D366..0x49D384` turns into the shooter's slot at `+4 + slot·0x1C` and writes through, is
  unbounded in stock; the extra-weapons module's splice at `0x49D364`, a row of the same table,
  bounds it by the unit's own count.
- `0x424575`: the weapon's sender takes the byte from the weapon's pointer and marks a hit whose
  byte is `0xFD`..`0xFF` with bit 11 of x (`0x800`), which no reachable cell uses. The receiver, the
  dispatch table's `0x0F` slot `0x455FB8` pointed at our stub, which continues at `0x455F50`, reads
  `0xFD`..`0xFF` as a sentinel only without the bit, drops a flagged byte below `0xFD`, and refuses a
  cell `0x481550` does not find. The sentinel senders stay stock's, so with weapons below 253 every
  `0x0F` is stock's byte for byte. A peer on another build reads a flagged x as a cell off the
  map: stock's `FeatureDie 0x423550` reads the NULL `0x481550` returns for it at `0x423568`
  (`0x4233A0`, the burn, tests it). Flagging the sentinels would reach that in every game; flagging
  the hit reaches it only for a weapon 253 or 255, and any hit from 256 up reaches `0x4244CF`
  the same way through its high bits. Every peer runs the same build.

**MEASURED**, two peers on Two Continents (`tools/weaponids_fixture.py`, lasers with the IDs
253–255 and a blast of 96 against wrecks, the joiner firing): the host read every hit of the
joiner's lasers as the weapon's, 140, 141 and 151 of them, and the joiner read the host's
unflagged broadcasts, 107 `0xFD` and 10 `0xFE`, as sentinels; paused after the fire stopped,
both peers held the same wrecks cell for cell and the same units at the same health. The
stock-limits build, on the fixture's `--low` half, the same. The previous build, on the same
`--low` run of 60 s: the host held 35 feature cells and the joiner 87, the three wrecks the
lasers hit gone from the host alone.

### Area damage's victim lists — `0x49A120`, its lists `0x49A262` and `0x49A5CE` [DISASSEMBLED + MEASURED 2026-09-25]

**The function.** Area damage `0x49A120(proj, at)` is `__stdcall`, `ret 8`, with an 8-aligned frame
(`and esp,0xfffffff8` at `0x49A123`). It has exactly two callers: `0x49A0A9` in `0x499EB0` (a
projectile's detonation, itself called from ten sites: `0x49A764`, `0x49AFEB`, `0x49B074`,
`0x49B1A9`, `0x49B213`, `0x49B275`, `0x49B3CD`, `0x49BA6F`, `0x49BACC`, `0x49BC70`), and `0x49A109` in
`0x49A0C0`, whose one caller is the fire spread `0x423BE0`. No literal reference to `0x49A120`
exists anywhere in the image [call and literal scan]. Its radius is the weapon's `+0xD6` halved
(`0x49A149`). It walks the cells of the blast's rect once, and for each cell takes its two unit
slots (`0x49A214..0x49A421`, the selector `[esp+0x10]` stepped at `0x49A415`, `jle` while it is at
most 1), then the cell's feature (`0x49A427..0x49A62B`), then steps to the next cell (the cell
pointer `[esp+0x14]`, stride 13, `0x49A62F..0x49A645`). The shooter itself is skipped (`0x49A259`,
`cmp esi,[ebx+0x52]`). After the walk a weapon with `+0x111` bit
30 (`0x49A66F`) detonates the projectiles inside its blast through `0x499EB0` (`0x49A764`), so the
function can run inside itself, and sends two `0x0E` (`0x49A7A8`, `0x49A7E9`).

**The defect.** Each walk keeps a list on the stack so that a victim standing on several cells is
hit once: 20 unit pointers at `[esp+0xA0]` (count `[esp+0x98]`), 64 anchor cells at `[esp+0xF0]`
(count `[esp+0x9C]`). Each list records only while it has room (`0x49A28F` `cmp ecx,0x14`,
`0x49A5FA` `cmp esi,0x40`), and the damage runs whether the victim was recorded or not: the unit
block falls through to its damage at `0x49A2AA`, the feature block to its call of
`0x4244B0(cell, x, y, weapon)` at `0x49A615..0x49A626`. So a unit found after the twentieth, or an
anchor after the sixty-fourth, is damaged once per cell of it in the rect — or, on a peer that is not
the host, a feature hit is sent once per cell ("Who sends a feature hit", above). For a feature
the repeat count is its cells whose own position is inside the radius, not its cells in the rect:
at `0x49A4BB` the flags and `+0x0A` come from the current walk cell, so an anchor is measured from
its record's position (`0x49A850`) and a `0xFFFE` cell from a position derived from the cell itself
(`0x421EB0`). The anchor's coordinates reach `0x4244B0` from `0x49A457..0x49A46F` (`x − [cell+0x0B]`,
`z − [cell+0x0A]`).

**MEASURED, previous build** (`scenarios/b1-victim-units.json`: 36 CORFLAK, 2×2, in a mirror-symmetric
ring round a self-destructed ARMVADER, CRAWL_BLAST): the row-major walk meets exactly 20 of them
first, and each lost one hit (222–304); each of the 16 found after the twentieth lost exactly four
times its mirror mate's loss, the 2×2 footprint's cells in the rect. `scenarios/b1-victim-features.json`
(96 `armcs_dead`, 5×5, 24 000 HP, round a self-destructed ARMCOM, a feature taking 9 999 a call):
the first 64 in walk order stand at 9 999; of the 32 after them, 30 were destroyed and 2 stand at
9 999, the two whose other cells all lie beyond the radius.

**The fix** (`fix_victim_caps`, simulation, fail closed). Both calls of `0x49A120` become calls of
`dmg_area`, a `__stdcall` of the same shape that makes a seen-set frame, calls `0x49A120` and
drops the frame, handing back `eax`. The unit block (72 bytes, `0x49A262..0x49A2A9`) becomes `push esi; call
dmg_unit_seen; test eax,eax; jnz 0x49A415; jmp 0x49A2AA`; the feature block (71 bytes,
`0x49A5CE..0x49A614`) becomes `mov eax,[esp+0x10]; push eax; call dmg_feature_seen; test eax,eax; jnz
0x49A62B; mov eax,[esp+0x10]; jmp 0x49A615`. A frame holds two hash sets, unit slots and anchor
ordinals, with no capacity; the damage math, the order of first hits
and every value stay stock's. Each call sits exactly where stock's list is consulted, so below the
caps every record and every skip is stock's: the unit block runs on the first cell a unit is found
on (after the shooter's skip, before its distance and damage at `0x49A2AA`), and the feature block
only for a cell that passed the distance test at `0x49A5C8` — a feature whose first cell fails the
test is recorded at its first cell that passes, as stock records it. No branch from outside lands
inside either block [rel8/rel32 scan of `.text`]; `ebx`, `esi` and `edi` are preserved by the callee, and `eax`, `ecx` and `edx` are dead at
every exit, as after stock's blocks.

**The invariant**: one explosion damages a unit at most once and reports a feature at most once,
and every index is bounded before use. A unit is recorded by its slot, `(unit − begin) / 0x118`,
remainder 0 and below the array's count (`u16 main+0x14351`); an anchor by its ordinal,
`(cell − grid) / 13`, remainder 0 and below `W·H` (`main+0x14233`, `+0x14237`). A pointer that fails
either is not damaged, and is counted. **Per thread, by a lifetime**: a call's frame is a local of
its wrapper, on the calling thread's own C stack, and the innermost frame is a TLS slot whose
previous value the wrapper keeps and restores, so a list block reads the innermost call's frame on
its own thread whatever another thread runs. `0x499EB0` is also reached from the `0x0E` receiver
(`0x49AFEB`), which runs on the game thread only: its receive mask is 4 (`0x45200B`), which the
dispatcher passes only in net state 6 (`0x454762..0x45477D`), set at `0x498445` after the load
([which thread](network.md#the-network-receivers-take-a-unit-index-straight-off-the-wire-0x09-0x0b-0x0c-0x2c-disassembled-2026-09-25)).
The frames stay per thread: that costs nothing and asks nothing of who calls area damage.
`0x499EB0` also skips a projectile whose owner's player record (`main+0x1B63 + 0x14B·owner`) has
`+0x73 == 3` (`0x49A01B..0x49A047`) [INFERRED: a remote player]. The sets live in the frame
(64 slots each, so 32 keys before one grows) and grow through `0x4D83B0`, whose failure is the engine's own out-of-memory exit.
Both calls and both blocks are rows of the one fail-closed table and nothing else calls
`0x49A120`, so a block never runs without a frame; if one did, it would be counted and logged once
and would damage as though the victim were new. The unit set is what B2 serves stacked aircraft
through.

**MEASURED, new build** (the raised build; the stock-limits build gave the same HP and counter on the
ring): every CORFLAK of the ring lost one hit, each of the 16 past the twentieth now equal to
its mirror mate, and the unit-repeat counter read 48, the 16 victims past the cap found three more
times each; all 96 wrecks stand at 9 999, and the feature-repeat counter read 602. The refused and
unframed counters stayed 0.

**MEASURED, two peers** — the feature half, since every non-host peer reports a feature hit with
`0x0F` instead of damaging it. A host and a joiner on Two Continents (`tools/mp_lobby.sh`), both
loading `scenarios/b1-victim-features-wrecks.json` and the joiner `b1-victim-features-blast.json`;
the joiner's ARMCOM self-destructed, every peer paused, the 96 anchors read on both. Previous build
(`d87e506`): on the host the first 64 in walk order stand at 9 999 and of ranks 65–96, 30 were
destroyed and 2 stand (ranks 75 and 96, only their anchors inside the radius); the joiner lost the
same 30, its own damage fields all 0 — a joiner never damages a feature, it sends `0x0F` and the
host's result comes back. New build (`629cfc7`): all 96 stand at 9 999 on the host and all 96 stand
on the joiner; the joiner's feature-repeat counter read 602 and every other B1 counter was 0 on
both peers.

### Flak's two divides — `0x49CDE0`, `0x49CE6A`, `0x49CF19` [DISASSEMBLED + MEASURED 2026-09-25]

**The function.** The ballistic fire `0x49CDE0` is called from `0x49D0F8` (the fire dispatch),
`0x49D44E` (the `0x0D` receiver) and `0x49D76E`, each only for a weapon with `+0x111` bit 1
(ballistic). It divides twice.

- `0x49CE6A` `div ecx`, `ecx` = `w+0x68`, for every shot. The loader stores `weaponvelocity` there
  scaled by 65 536/30 (`0x42E4C6` `fmul [0x4FD240]`, the double 2 184.533…; stored at `0x42E4DC`),
  and nothing else writes it. A ballistic weapon with `weaponvelocity` 0 faults here; no stock
  weapon has one.
- `0x49CF19` `idiv ebp`, only for a `burnblow` weapon (`+0x111` bit 23, tested at `0x49CECC`): the
  flight time is the horizontal distance over `ebp`, which is `TurnZLookup 0x4B7123(pitch, v)` =
  `v·cos(pitch)` from the 512-entry table at `0x509F00`, 0 at entries 0 and 256 — a pitch within
  0.35° of straight up or down. The non-burnblow branch `0x49CF29` takes `weapontimer`, `w+0xE6`,
  instead, with `eax` the weapon as `0x49CEC6` left it. Stock's flak guns (ARMFLAK_GUN,
  CORFLAK_GUN, ARMYORK_GUN, CORSENT_GUN) are ballistic and burnblow, and the `0x0D` receiver runs the
  same code, so every peer that receives such a shot would divide.

**The loader's closing call.** `0x49E010(weapon)` (`ret 4`) picks the weapon's aim routine at
`w+0x60` from its flags: bit 19 → `0x49D580`, bit 4 → `0x49DB70`, bit 0 or bit 20 → `0x49D9C0`, bit 8
→ `0x49DD60`, and otherwise leaves it. Its only callers are the loader's two exits, `0x42F314` and
`0x42F32E`.

**MEASURED, previous build** (`scenarios/b1-flak-overhead.json`): the divide was **not reproduced**.
An AI CORFLAK never aimed at an ARMATLAS held exactly overhead, nor at one 30–40 wu to the side at
67° and above, in 30 s; at 31° it engaged at once. An ARMBRAWL flown straight over it raised the aim
to 28.9° and froze it while overhead. The highest pitch seen while firing was 29.6°. So stock
acquisition does not approach the zero band in these runs, and the fix rests on the disassembly.
Where acquisition cuts off was not disassembled.

**The fix** (`fix_flak_divides`, local). `0x49CF18..0x49CF20` (`cdq; idiv ebp; mov edx,[0x511DE8]`)
jumps to a stub that divides as stock when `ebp ≠ 0` and returns to `0x49CF21`, and otherwise takes `weapontimer` through
`0x49CF29` and counts the fallback (logged for the first 8 and every power of two). The loader's two
calls of `0x49E010` go through a stub that first gives a ballistic weapon whose `w+0x68` is 0 the
value 1, and logs it. The floor also changes the pitch solver's input — `0x49A890`, handed `w+0x68`
at `0x49D608` — but only for such a weapon, which exists only in a mod and which stock faults on
the moment it fires; that is why the fix stays local. No branch from outside lands in `0x49CF19..0x49CF20` but stock's own
`0x49CF15` and `0x49CF8F`, which the scan's byte-level decoding reports and the disassembly shows are
operands, not branches. **The invariant**: no divide in `0x49CDE0` sees a zero divisor. It is local:
a peer without it faults on the shot rather than diverging silently. MEASURED on the new build: the
flak shot down an ARMATLAS moved beside it (pitch 4 331), and no fallback was counted.

### Stacked aircraft in area damage — `0x49A664`, `0x49A415`, `0x47CF98`, `0x4954ED` [DISASSEMBLED + MEASURED 2026-09-25]

**Slot B.** The stamp `0x47CC30` tests `+0x110` bit 29 first (`0x47CD5A`): with it set, the unit
takes slot A `[cell+0]` of the cells its def's yardmap `+0x14E` names (read at `0x47CD82`).
Otherwise it files the unit by `+0x110 & 3` (`0x47CEBC..0x47CEC2`): 1 in slot A, 2 (airborne)
in slot B `[cell+2]` through `0x47CF98` (`cmp eax,2; jne 0x47D0D5`, the stamping continuing at
`0x47CFA1 test edi,edi`); any other value stamps no cell. All three paths contest a held cell the
same way, the slot-A ones at `0x47CDD5` and `0x47CF0B`, slot B's as follows. For each cell of the footprint
(`0x47CFB3..0x47D042`) an empty slot B takes the unit. A held one is contested at `0x47CFDA`: when
the incumbent's player record (`+0x96`, dereferenced unconditionally) has a non-zero first dword
(`0x47CFE0 cmp dword [ecx],0`) and a `+0x73` of 3 (remote human), the newcomer takes the cell (`0x47D010..0x47D03A`); otherwise the incumbent keeps it
(`0x47CFEB..0x47D00E`). Either way the unit left without the cell gets `+0x110` bit 27
(`0x8000000`) and the holder bit 26 (`0x4000000`). Area damage reads only the two slots of each
cell (above), and so does the direct-hit test `0x49B090`, so an aircraft no in-rect slot names is
found by neither: one holding no cell, or holding cells only outside the rect while others hold
the ones inside it.

**The walk's end.** The row loop falls through to `0x49A664` (`0x49A65E jl 0x49A1E9` not taken), and
an empty rect jumps there from `0x49A1DF` (`jge`). `0x49A664..0x49A66E` is `mov eax,[ebp+8]; mov
ecx,[eax]; mov edx,[ecx+0x111]`, the weapon's flags for the tail's bit-30 test at `0x49A66F`, and
the tail reloads every register it reads. The selector step `0x49A415..0x49A426` is `mov
eax,[esp+0x10]; inc eax; cmp eax,1; mov [esp+0x10],eax; jle 0x49A214`; it falls through to the
cell's feature at `0x49A427`. Every path out of the unit block that starts at `0x49A24E` — the NULL
test, the shooter's skip `0x49A259` (`cmp esi,[proj+0x52]`), B1's seen-set skip, the damage and its
reload `0x49A411` — comes back to `0x49A415`, and none of them writes `[esp+0x10]`. No branch from
outside lands in `0x49A665..0x49A66E`, `0x49A416..0x49A426`, `0x47CF99..0x47CFA0` or
`0x4954EE..0x4954F1` [rel8/rel32 scan of `.text` and an absolute-dword scan of the image: two hits,
`0x49A642 → 0x49A668` and `0x49A6DE → 0x49A66B`, are the modrm and the displacement of two `mov`s,
not branches].

**The step.** `0x4954ED` is the sim step `0x495490`'s call of the unit tick `0x48AD30` (`E8 3E 58 FF
FF`), its only caller; the projectile tick `0x49B720` follows at `0x495513`, also its only caller.
A unit's death explosion runs inside the unit tick, a projectile's detonation inside the projectile
tick.

**MEASURED, previous build** (`b2-stack-atlas`, ten ARMATLAS ordered to (1600, 1600) on Two Continents,
then `b2-aa-flak`, an idle AI's CORFLAK 350 east, applied at GameTime 360; a scratch census of every
airborne unit's footprint cells whose slot B holds its own `+0xA8`, each tick): up to seven of ten
held no cell. The CORFLAK's first burst (GameTime 429) took the four holders from 150 HP to 5–11
and left the five holding none, each within a cell of them, at 150; its second (450) again hit
only holders, and three of the five were still at 150 holding none. Across both bursts, 12 hits,
all on holders.

**The fix** (`fix_stacked_air`, simulation, fail closed, air only). It serves the aircraft stock
missed to stock's own per-victim code *after* the walk, so stock's victims, their order and their
values are untouched, and the new ones go through stock's shooter skip, B1's seen-set (which
records them), distance test, falloff and damage with no arithmetic of ours:

| site | stock | the stub |
|---|---|---|
| `0x49A664` | the 11 bytes above | `push [ebp+0xC]; push [ebp+8]; call air_first`; a unit → `esi`, `[esp+0x10] = 2`, `jmp 0x49A24E`; none → the 11 bytes and `jmp 0x49A66F` |
| `0x49A415` | the 18 bytes above | a selector below 2 steps exactly as stock (the flags `jle` reads are stock's: `inc`, `cmp`, then a `mov`) and leaves for `0x49A214` or `0x49A427`; at 2, `call air_next` and `0x49A24E` again, or the 11 bytes and `0x49A66F` |
| `0x47CF98` | `cmp eax,2; jne 0x47D0D5` | the same test, then `pushad; push esi; call air_note; add esp,4; popad; jmp 0x47CFA1` |
| `0x4954ED` | `call 0x48AD30` | `call` to `pushfd; pushad; call tagpu_replay_capture; call tagpu_rec_capture; call air_rebuild; popad; popfd; call 0x48AD30; pushfd; pushad; call tagpu_replay_step; popad; popfd; ret`, which returns to `0x4954F2`: the replay's capture-point write and the demo recorder's capture first, the replay's step after the unit tick ([*tick boundaries*](sim.md#the-demo-recorders-tick-boundaries-the-sim-step-0x495490-and-its-callers-disassembled-2026-10-01)) |

`air_first` builds the call's candidate list, in the frame B1's wrapper `dmg_area` already holds
(32 slots inline, then a block from `0x4D83B0` freed when the call returns), from a pool of airborne
slots: the rebuild refills the pool at every step with every unit whose `+0x110 & 0x30000003` is
`0x10000002`, and the stamp's airborne path adds each unit it files. A slot is a candidate when it is
below the array's count (`u16 main+0x14351`) and its unit

- is alive and airborne by slot B's own rule: `+0x110 & 0x30000003` is `0x10000002`, since the stamp
  sends a unit with bit 29 down its yardmap path to slot A (`0x47CD5A`) whatever its `& 3`;
- is in the grid and not dying: its bucket `+0x82` neither NULL nor the off-map bucket
  `*(main+0x142B7)`, and no bit 14 (pending death). Stock never offers a dying aircraft to its own
  death explosion: the destructor's grid clear `0x47CBD0` (called at `0x48682D`) empties its cells
  and sets `+0x82` to NULL (`0x47CC19`) before the explosion `0x49B000` it calls at `0x486D50`, and
  that explosion's projectile has no shooter (`proj+0x52` = 0, `0x49B03E`), so the shooter's skip
  `0x49A259` would not keep it out. The bucket test excludes it in every case, since the clear
  always runs first. Bit 14 alone would not: the damage receiver skips that write when the owner's
  player record has a zero first dword (`0x489ECC`), and the owner gate `0x49A03F..0x49A047` then
  lets the explosion's area damage run (its projectile carries the dying unit's player,
  `proj+0x66`, set at `0x49B055`); and a death that does not come through `0x489CE0`
  (`Send_UnitDeath`'s direct callers) need not set it. On a peer that does not own the unit, the
  gate skips the explosion's area damage whatever the bits;
- is uncarried (`+0x86` 0) and has its model (`+0x9E` not NULL, the death guard, `0x4866D0` below);
- has a footprint (`+0x76`, `+0x78`, `+0x7E`, `+0x80`) at least one cell each way that meets the
  blast's rect, and is not in the call's unit set.

The rect is `0x49A120`'s own: the
radius `(u16)w[+0xD6] >> 1`, `radius/16 + 1` cells about the blast point's cell (`at+0x02`, `at+0x0A`,
each truncated /16), the low ends raised to 0 and the high ends lowered to `W`, `H`
(`0x49A149..0x49A1C7`). `air_next` re-tests every one of those on the live unit when it hands it
over, so a unit the damage before it killed, or whose slot a new unit took, is re-judged.

**The invariant** (a bound, a lock and a lifetime): every unit handed to the engine was validated
in this call, at hand-over, and every index was bounded first — a pool slot below the count, the
pool below 65 536 entries because a slot enters it at most once (a membership byte kept with it).
The pool is written by the rebuild on the game thread and by the stamp, which also runs from the
network pump and from a saved game's restore on the loader thread, and read by `air_first` on
whichever thread runs area damage: one critical section covers every read and write of it, held
only inside our C, the one engine call under it being the allocator `0x4D83B0`, which takes its own
critical section after ours and never the reverse. The pump at `0x4954C8` runs before the rebuild,
so an explosion from its `0x0E` receiver sees the previous step's pool plus the units stamped since:
a stale pool can only miss a victim, since every hand-over is decided on the live unit. **A failed
allocation** is the engine's out-of-memory exit whatever the handler slot holds: `0x4D83C0` calls
the handler at `[0x5289BC]` (`0x4D8409..0x4D8412`) and returns NULL when the slot is empty, and the
slot's setter `0x4D8E50` clears it around some of the engine's own allocations (`0x495ABE` until
`0x49E6F0` puts `0x49E700` back from `0x495AFD`; `0x4B3B75`..`0x4B3B8F`; `0x4B4146`..`0x4B422B`), so
another thread's failure inside such a window returns NULL; `eng_alloc_or_exit` then calls
`0x49E700` itself, which ends the process. B1's seen-sets grow through it too. A call's list lives
in its frame, so the nested call from the interceptor tail `0x49A764`, which runs after the serving,
has its own. **What it can miss**: a unit whose `+0x110` turned airborne since the last rebuild
through a writer not followed by the stamp — 150 instructions write `+0x110`, too many to prove
each one is — holds no slot either, and stays stock's until the next rebuild. **The counter** counts aircraft handed to
the engine, not aircraft damaged: stock's own filters (the distance test above all) still apply.

**MEASURED, new build**, the same fixture and timing: the first burst (429) took all ten from 150 HP to
10–16, six of them holding no cell at the damage tick; the second (450) killed all ten, five holding
none. The counter read 40 in that run and 37 on the stock-limits build. After B1's landing was
merged in, with the candidate rule above (bit 29, bit 14, a real bucket): the raised build installed
183 sites, the stubs (the engine fixes' and the limits' weapon sites) 3 504 bytes in one page; the first burst took all ten from 150 to 11–17, the
second killed them, the counter read 10; the stock-limits build installed its fixes' 57 sites, stubs
3 120 bytes, every fix armed or in the table, to the menu.

**MEASURED, cost** (`scenarios/air-war.json`, 200 aircraft over two bases, 2 100 ticks, scratch
timers around our functions in the raised build): the rebuild 18.5–23.4 µs a tick (it scans every
slot to the count), `air_first` 0.6–0.8 µs an area-damage call, 40–73 stamp notes a tick; about
25 µs a tick in all, against a unit tick of 240–530 µs in the same runs. The battles themselves
diverge between builds (the fix kills aircraft sooner: 122 units on the roster at the end against
240), so whole-tick times are not a comparison.

**MEASURED, two peers** (`tools/mp_lobby.sh`, both on the new build; the stack applied on the host,
a CORFLAK on the joiner): the firer's peer computes a hit and the victim's owner applies it
(`0x489BB0` sends the `0x0B`), so the joiner's pool is the one that serves. The joiner served 1503
and 1509 at its GameTime 839 and the host applied both (150 → 19) at its 851, among nine hit. A
remote unit's HP word `+0x108` reads 0 on a peer that does not own it, before any damage and after
(and `+0x104` reads 1.0), so there is no second HP to compare: the host's is the only one. Paused
after the fight, both peers held the same five atlases (1502, 1503, 1505, 1509, 1510) and the
CORFLAK. **One caller is not gated by the owner**: the fire spread `0x49A0C0` (from the feature
tick at `0x423BE0`) builds a zeroed projectile on its stack with owner 10 (`proj+0x66`) and no
shooter (`proj+0x52` = 0) and calls `0x49A120` itself (`0x49A109`), past `0x499EB0`'s local-owner
test, so every peer computes a burning feature's damage. Stock already does that for the slot
holders, which each peer picks by its own tie-break (`0x47CFDA`: a remote incumbent loses the
cell); B2 extends it to the stacked aircraft, and since it serves every airborne unit in the rect
that no in-rect slot names, the set damaged is the same whichever unit each peer's tie-break kept.

## The weapon decisions and a hit's alerts — mapped for TADR section C, landing C2 [DISASSEMBLED + MEASURED 2026-09-25]

Every address the weapon keys and `nomapweaponalert` touch or read
([data keys, C2 as built](../tadr-port/data-keys.html#c2-as-built); the hooks are in
[gpu-status §2.97](../gpu-status.html)). A rel8/rel32 scan of `.text` finds no branch landing
inside any of the fourteen sites' replaced bytes (`0x42E311`–`15`, `0x49ABB1`–`B6`, `0x43F1D5`–`D9`,
`0x49B9EC`–`F1`, `0x49E1FE`–`E202`, `0x489D8F`–`92`, `0x489DA3`–`A6`, `0x4071D9`–`DC`,
`0x466EBA`–`BE`, `0x49E011`–`19`, `0x48797C`–`82`, `0x4872CD`–`D1`, `0x499E38`–`3B`,
`0x489C8A`–`8D`), and no absolute pointer in the image names one.

### The can-engage test `0x49ABB0(unit, target, idx)`

`stdcall`, `ret 0xC`; the weapon is `[unit + 0x10 + idx·0x1C]`; entry `8B 44 24 0C 83 EC 0C`. Eleven
callers: acquisition `0x40B914`, retaliation `0x4070D0`/`0x407125`/`0x40714F`, the attack cursor
`0x43E59E`, the order handlers `0x402302`, `0x4035D6`, `0x4037D7`, `0x406461`, `0x40FE07`, `0x413908`.

- **Water path** (weapon `+0x111` bit 16) has exactly three exits: `0x49AC0F` returns 0 for a target
  that is not a `floater` (def `+0x241` bit 19) with `y` (`+0x70`) above the sea; `0x49AC3B` returns
  0 for a `canhover` target (bit 12) with `y + height/2` (def `+0x170`) above it; and the range test
  `0x49AC47..0x49ACA2`, the high words of `dx²` and `dz²` (16.16 positions `+0x6A`, `+0x72`) summed
  against range² (weapon `+0xDC`). "Above" is `y > sea`, so a building standing at the sea level
  (a spawn at `height` 75 on a sea of 75) is not a surface target.
- **Other weapons**: the shooter's top at or below the sea returns 0 (`0x49ACD4`), the target's
  top likewise (`0x49ACFB`); `toairweapon` (bit 17) needs a flying target (`0x49AD07`,
  `+0x110 & 3 == 2`); a ballistic weapon returns 0 when `0x49A890` answers `0x8000`.
- So turning any 0 from the water path into the range test is exactly TADR's `surfacefire`
  redirect of the two rejections.

**Can-aim `0x49AA80(unit, &from, &to, idx)`**: the range test first, then a water weapon returns 1
at once (`0x49AB26`); other weapons test a submerged firer and the ballistic reach. It has no air
test, and re-validation `0x4089A0` never re-asks `0x49ABB0`.

### The attack cursor `0x43E545` and the order action `0x43F0E0`

- **The cursor mapper's attack case** `0x43E545`: a candidate with def `+0x245` bit 4 and a weapon
  with bit 8 → 2; `[candidate+0] != 0` → 1, the attack cursor, without asking (INFERRED: a mobile
  unit); otherwise `0x49ABB0(candidate, hovered, 0)` → 1, or 3 "too far". The cursor index is the
  byte `main+0x2CBE` (1 attack, 3 too far, 15 select, 19 normal) and the unit under the pointer
  the `u16` `main+0x2CBA`. MEASURED: over targets out of range, keyed and stock towers both show 3.
- **The order action** `0x43F0E0` (`ScriptAction_Type2Index` in the symbol set) makes the
  right-click's order: called from `MOUSE_EVENT_2UnitOrder 0x48CF30` at `0x48D0A0`, and at
  `0x4804A3` in a loop over a group. It does not ask `0x49ABB0`. Its unit branch starts at
  `0x43F1D4` (`mov eax,[edi+0x110]`, reached only from `0x43F17C`, on its first byte): `edi` the
  target, `ebp` the shooter, weapon 0 at `+0x10`, slot 1 at `+0x2C` when `+0x3B` bit 1 is set.
  It refuses a ground target for a `toairweapon` weapon 0, tests a submerged target against both
  slots' water bit (`0x43F21B`) and refuses a hovercraft's water weapon 0 (`0x43F24F`). Its exits
  are the refusal `0x4401DC`, no action `0x43F26C` (a hover whose slot 1 is a water weapon) and on
  `0x43F27A`, where `edx` holds the target's def from `0x43F1F1` for `0x43F2AA`.
- **Order nodes**: the list head at unit `+0x5C`, the type at node `+0x4`. MEASURED: a laser
  tower's standing order is 22 and its attack order 8; a hovercraft idle is 41, and its attack
  order 6.

### AutoAim's slot loop `0x49E1A0`, and the fire functions

- Per slot: the target read `0x48A1E0` at `0x49E1E1` (it drops a dead target and runs
  `TargetCleared`; stock clears the aiming bit at `0x49E1EA` for a lost one), the fire-function
  test at `0x49E1F2`, then `0x49E1FD` (`mov eax,[ebx+0x111]`, reached only from `0x49E1F7`'s fall
  through): `ebx` the weapon, `edi` the unit, `esi` the slot's state byte, `[esp+0x10]` the slot
  index. Bit 19 takes the turret's aim path; otherwise bit 4 (`vlaunch`) starts the aim script
  (`0x4B0A70`) and sets the state's bit 0. At reload 0 it then asks `0x49AA80`, the stock and
  energy (the player `[edi+0xEC]`: `+0x8C` against weapon `+0xC0`, `+0x98` against `+0xC4`),
  `0x48A190`, and calls the fire function `[ebx+0x60]` at `0x49E43C`, its only call site. The next
  slot is `0x49E541`.
- `ClearTarget 0x48A0F0(unit, slot)`, `stdcall`.
- **`0x49E010(weapon)`**, `ret 4`, entry `8B 4C 24 04 8B 81 11 01 00 00`: sets `+0x60` from the flags
  (bit 19 → `0x49D580`; bit 4 → `0x49DB70`; bit 0 or 20 → `0x49D9C0`; bit 8 → `0x49DD60`). Its
  only callers are the loader's two exits, `0x42F314` and `0x42F32E` (both through flak's stub).
  At both, `+0x111` and the default damage are final. The default damage `+0xD4` is a **WORD**:
  read at `0x499CE3` (`mov cx,[eax+0xD4]`), written at `0x42EFA9` (the `[DAMAGE]` section's
  `default`) or `0x42F326` (0, no section).
- **`0x49DB70`**, the vertical-launch fire, fires only when the slot's `+8`, the aim script's
  result, is non-zero. A unit whose COB has no `AimPrimary` never fires such a weapon. MEASURED:
  `ARMSUB`'s scripts are `Create`, `StartMoving`, `StopMoving`, `FirePrimary`, `QueryPrimary`,
  `SweetSpot` and `Killed`, and it never launched until one was added.
- **The guidance `0x49B9EB`** (`test eax,0x10000; je 0x49BA16`, `esi` the weapon): in
  self-propelled flight a water weapon above the sea falls under gravity (`0x49B9C2..0x49BA11`,
  `main+0x14263`) and does not steer.
- **The weapon load `0x42E310`**: entry `sub esp,0x120`; its one caller is `0x4918BB` in
  `LoadGameData_Main 0x4917D0`; the ID site `0x42E468` is A′3's. It empties the 256 slots
  (`0x42E31C..0x42E343`: the name's first byte 0, `+0x10A` the slot's own index), lists
  `Weapons\*.tdf` (`0x42E371`), and gives every section of every file to `0x42E440`, which takes
  the slot from the section's `ID` (`0x42E463`, default −1), copies the section's name into the
  slot's first bytes with no bound (`0x42E490..0x42E4A9`) and its `name` into `+0x20`
  (`0x42E4B1`, 0x40 bytes). A later section with the same `ID` overwrites an earlier one. Nothing
  else fills the table, so the shell holds none until a level has loaded: **MEASURED
  2026-10-02** on retail, at the main menu of a fresh start, every one of the 256 names at
  `main+0x2CF3` was empty.
- `0x485070(pos)`, `ret 4`: the ground height under a position, −1 off the map.

### A hit's alerts, its blink and the stones' dots

- **`0x489CE0`**, the hit record's applier (`edi` the 9-byte record: `+1` victim id, `+3` attacker
  id or 0, `+5` amount, `+8` kind). Two callers: `0x489C89` (local, in `0x489BB0`) and `0x455412`
  (the dispatcher's `0x0B` case). It pushes `ebx`, `esi`, `edi` and nothing else before its calls.
  For any kind but `0xA` (heal): `push esi; call 0x467950` at `0x489D8E`; kind `0xB` then skips to
  `0x489DA7` (`0x489D93`); otherwise `push amount; push victim; push attacker; call 0x406F80` at
  `0x489DA2`. From `0x489D93` and `0x489DA7` on, nothing reads `eax`, `ecx` or `edx` before writing
  them. MEASURED on two peers: attacker-less meteor hits reach it by both callers.
- **`0x489BB0(attacker, victim, amount, kind, angle)`**, `ret 0x14`, the hit's sender: it builds
  the 9-byte record on its own stack (`[esp+0xC]`: `+0` `0x0B`, `+1` the victim's id, `+3` the
  attacker's or 0, `+5` the amount after armour, `+7` the angle, `+8` the kind), applies it
  locally with `push edx; call 0x489CE0` at `0x489C89`, then sends it to a remote victim's owner.
  Before `0x489C89` it calls only the arithmetic helper `0x4E43D0` (`0x489BEC`) and has no exit.
  Every weapon-kind hit comes from one of its twelve calls, **`0x499E37`** in the damage function
  `0x499CD0`: attacker `[proj+0x52]` (`0x499D9D`), kind `1 + weapon bit 7` (`0x499E1E..0x499E32`,
  where `[proj]`, the weapon, is read), the angle relative to the victim's heading; the
  projectile is at `[esp+0x30]` at the call. After both calls nothing reads `eax`, `ecx` or `edx`
  before writing them (`0x499E3C`, `0x489C8E`).
- **Attacker-less weapon hits** come from three sources: a meteor's projectile (the pool's only
  attacker-less kind), a unit's death explosion `0x49B000`, which zeroes its stack projectile's
  `+0x52` (`0x49B03E`), and the fire spread `0x49A0C0` (a zeroed stack projectile). The record
  cannot tell them apart. MEASURED 2026-09-26: a `CORFUS`'s `ATOMIC_BLAST` and a solar
  collector's `SMALL_BUILDINGEX` reach `0x499E37` with attacker NULL and kind 1. In a two-peer
  game each peer computes a stone's hit on every unit itself, and the victim's owner also
  receives the other peer's copy.
- **`0x467950(unit)`**: `mov byte [unit+0xFA],0xF0`, `ret 4`; its one caller is `0x489D8E`.
- **`0x406F80(attacker, victim, amount)`**, `ret 0xC`; its one caller is `0x489DA2`. With no
  attacker it jumps to the notification (`0x406FFC`), which runs when `0x438BE0(victim) & 0x80` is
  clear and `+0xF4 ≠ +0xFF` or `+0xF5 == 1` (`0x4071B0..0x4071D1`). `+0xF5` still holds the previous
  hit's kind there (stored at `0x489DAC`, after the call). `+0xF4` is written by the create
  (`0x485C83`, 10), by a hit with an attacker (`0x489DC0`), at `0x486787`, by the saved game's
  restore (`0x48722C`), and by the key switch `0x4956C0` (10) for the unit under the cursor
  (`u16 main+0x2CBA`, `0x495780`, gated on `main+0x3923B` bit 1, which also sets `+0x110` bit 14
  and zeroes `+0xF0`); a fresh unit reads 10 there, so its first hit already alerts, attacker or
  not. The call is `0x47F850(victim, 2, 0)` at `0x4071D8`.
- **`0x47F850(unit, index, text)`**, `ret 0xC`: requires the unit off screen (`0x48BCB0` over the
  on-screen list `main+0x1435F`, count `+0x14367`), owned by `main+0x2A43`, `+0x110` bit 28 set and
  bit 14 clear; then `0x47FAD0` on the queue `[0x51E68C]`. The queue: 8 entries of 17 bytes
  (`+0` index, `+4` GameTime, `+8` unit, `+0xC` text, `+0x10` priority), count `+0x99`, an index
  already queued refused. The consumer is `0x47F680`, called once per main-loop pass (`0x4999A6`),
  which plays an entry through `0x47FD70`; `0x47FCA0`, which also calls `0x47FD70`, has no caller. The per-index next time `0x5086EC + 24·index` (`0x50871C` for "Under Attack") is
  written at `0x47FEFD` only when the player `0x47FD70`'s four gates pass -- priority above 10 −
  unitchat, a variant count above 0, sound asked for, speechfx -- and then whether or not fxvol,
  Sound Mode or NoDirectSound silences the play (*The unit replies, the named sounds and the
  player's sound settings*). MEASURED: with sound off it stayed 0 while the text repeated; which
  gate held it is not recorded, and fxvol 0 alone would not [the category's `underattack` count
  of 0, or speechfx off, would: INFERRED].
- **`+0xFA`, the recently-hit byte: its every writer** — the hit (`0x467954`), the unit tick's
  decrement (`0x48ADF0..0x48ADFC`, beside AutoAim's call `0x48ADDA`), the create (`0x485C12`, 0) and
  the saved game's restore (`0x4872CC`, from the save; written out at `0x487975`). `0x4225EC` (a feature
  def's `height`, in the feature loader) and `0x42E6D6` (a word of a weapon record) write the same
  offset of other records. Its readers include the order
  code (`0x404BFD`, `0x404D17`, `0x404ECA`, `0x405079`, `0x4149FC`) and the blink.
- **The blink**, in the radar rebuild `0x466DC0` (the game thread, every sim tick):
  `mov al,[ebx+0xFA]` at `0x466EB9` (6 bytes; `ebx` the unit, `esi` main); non-zero, the dot is
  drawn only when `main+0x142F1` bit 0 is set (`0x466EC3`), else skipped (`0x466F11`). That bit is
  flipped every 8 sim ticks by `0x466580` (a countdown at `main+0x142EF` from 7), called from the
  sim tick at `0x4955E5`. MEASURED: a hit unit's dot is missing in 3 to 5 of 8 shots.
- **The meteor weapon `[0x512328]`**; the shower's tick `0x437DE0` centres each shower at
  `rand()` over the whole map (`main+0x14237`, `+0x14233`, scaled by `/0x8000`), so a test's
  units need a small map to be under it. `+meteor` (`0x438070`) turns the shower on.
- **The meteor's dot**: weapon `+0x111` bit 6 (`noradar`), read only at `0x467206`.
- **A saved game's unit records.** The saver `0x4876C0(file)` has one caller, `0x432A01` in the
  game's save routine, and walks the unit array from `main+0x14357` to `main+0x1435B` in `0x118`
  steps, saving every unit with `+0x110` bit 28 (alive; `0x487701`): one record of `0xB8` bytes
  built at `[esp+0x18]` and written whole by `0x4B4CF0` (`0x487A9E`), then the count. Before each
  record it writes the unit's COB state under `Script%i` (`0x508C24`; `0x4B1EC0` on `+0x9A`,
  `0x487711..0x48773D`), each order of `+0x5C` and then of `+0x60` under `u%04xm%04x` (`0x508C30`;
  `0x43A970`, `0x487742..0x4877C2`), the mover through `0x43DD70` when `unit+0` is set
  (`0x4877C4..0x4877CD`) and the `+0xBC` sub-object through `0x4010B0` (`0x4877D2..0x4877DA`).
  `0x43A970` writes an order's record, then its action name and, for a MobileBuild,
  VTOL_MobileBuild or BuildingBuild order, the type name of `+0x36` when that is in
  `[1, UNITINFOCount)` (`0x43AAF3..0x43ABD4`) [DISASSEMBLED 2026-09-28]. The restore
  `0x487080(id, file)` (`ret 8`) returns at once when the unit at that id is already alive
  (`0x4870C3`); otherwise it reads records by index into its own `[esp+0x18]` (`0x4B4C80` at
  `0x48711D`, refusing any length but `0xB8`) until the id word `+0x21` matches, and creates the
  unit (`0x485F50`). After the create it restores the `+0xBC` sub-object (`0x401110`, `0x487530`),
  the mover when the record's flag `+0x27` is set (`0x43DE30`, `0x487541`), the record's `+0x23`
  orders — each built by `0x43A420` into a fresh `0x56`-byte node (`0x48757A..0x487594`) and linked
  at the tail of `+0x60` when it carries `0x40000`, else of `+0x5C` (`0x48759D..0x4875B8`), outside
  `0x43ADC0`'s rules — then `0x4388B0` on the `+0x5C` head, which hands its `+0x52` to the mover
  class's `[vt+4]` (`0x4875C4..0x4875CB`), and the COB state (`0x4B2040`, `0x487600`)
  [DISASSEMBLED 2026-09-28]. Its seven call sites all resolve a unit id inside the same load: `0x486FD0`
  (at `0x48705B`; itself at `0x432672` in the game-load routine `0x432610`, on the LOADER thread),
  its own recursion for the ids at `+0x89` and `+0x8B` (`0x4871DD`, `0x48720D`), and the order
  restore `0x43A420` (called at `0x487594`) with its helpers `0x44DE80` and `0x44E7D0` (`0x43A5ED`,
  `0x44DEE8`, `0x44DEF6`, `0x44E816`). `0x486FD0` reads every record but uses only `+0x21`, and
  accepts a record of `0xB6` bytes.
  Offsets in the record, as the two sides use them: `+0xAB` the unit's `+0xF5`, `+0xAC` `+0xF6`,
  `+0xAD` `+0xF7`, `+0xAE` the WORD `+0xBA`, `+0xB0` `+0xF8`, **`+0xB1` `+0xFA`** (out at `0x487996`,
  in at `0x4872CC`), **`+0xB2` a WORD, `movzx` of the byte `+0x10E`** (`0x48797B`), of which the
  restore reads only the low byte (`0x4872D2`), and `+0xB4` a DWORD of packed bits. So **`+0xB3`
  is 0 in every save stock writes and read by nothing**: the one spare byte, which
  `nomapweaponalert` uses to carry its blink hold (gpu-status §2.97). The saver's three weapon
  slots fill `+0x41..+0x88` (`0x487A0D..0x487A7D`), clear of it. MEASURED 2026-09-25: the saver
  runs on the game thread (the thread of the radar rebuild) and the restore on the loader thread;
  a loaded game starts paused; the unit array's base moves at the load, so an address read before
  it is stale; the records do not appear as plain bytes in the `.SAV` file (compressed
  [INFERRED]).

## Veterancy, a hit's word, the radar's owner test and the meteor shower — mapped for TADR landings C3 and B7 [DISASSEMBLED + MEASURED 2026-09-26]

Every address veterancy (C3) and section B's seventh item (B7) touch or read
([data keys, C3 as built](../tadr-port/data-keys.html#c3-as-built),
[simulation fixes, B7](../tadr-port/sim-fixes.html); the hooks are in [gpu-status §2.100](../gpu-status.html)).
A rel8/rel32 scan of `.text` and every dword of the image finds no branch or pointer landing inside
any of the replaced ranges: the seven veterancy sites, `0x489BF3`–`F9`, `0x489C71`–`75`,
`0x49A01B`–`1F`, `0x4673B1`–`BB`, the two panel sites, and the calls at `0x49DF7D` and `0x49D307`.

### A unit's kills and the seven places stock reads a level

Kills are the u16 `unit+0xB8`, zero-extended by every reader; stock's level is `min(kills / 5, 5)`
(`imul 0x66666667; sar edx,1`), except where noted.

| site | function, what it computes | registers at the site |
|---|---|---|
| `0x489BFA` (7 bytes, `mov cx,[esi+0xB8]`) | the hit sender `0x489BB0`: the victim takes `(25 − L)·4 %` of the amount after armour (`0x489C16..0x489C32`), result in `edx` | `esi` the victim, `edi` the amount; `edx` dead (the `imul` at `0x489C01` writes it) |
| `0x499DB5` (7, `mov cx,[ebx+0xB8]`) | the damage function `0x499CD0`: the shooter deals `(100 + 6L) %` (`0x499DD1`) | `ebx` = `[proj+0x52]`, the shooter, tested non-NULL at `0x499DAA`; `esi` the amount |
| `0x49E468` (7, `xor ecx,ecx; mov eax,0x66666667`) | AutoAim's reload: the slot waits `reload · (100 − 6L) % · (120 − 20·HP/maxHP) %` (`0x49E48D..0x49E4BB`), so a damaged unit reloads slower | `edi` the unit; `ecx` = L at `0x49E48D` |
| `0x48A324` (14, `cmp word [edi+0xB8],5; jbe 0x48A42D`) | target lead: on past 5 kills (`0x48A332`), off at `0x48A42D`; neither reads the flags | `edi` the unit |
| `0x49D6EA` (5, `mov eax,0x2AAAAAAB`) | the fire method's spread: the divisor `kills / 12`, applied when above 1 (`0x49D702`, `cmp ebx,1`); `ecx += 0x800` first | `edi` the unit, `ecx` the spread; `ebx` written at `0x49D700` |
| `0x4043D8` (7, `mov cx,[edx+0xB8]`) | the capture order `0x4042xx` (a state machine on `[order+5]`, jump table `0x404714`; "Capturing" `0x5013B8`, "That unit cannot be captured" `0x501618`, "…a cloud of vapor…" `0x5015E0`): the capture's cost, `min(f(def), 0x708)` (the clamp at `0x40438A..0x404396`) scaled by the target's HP (`0x404359..0x4043C7`; at most the clamped base, 1 800, for a target at full HP), times `(10 + L)/10` (`0x4043E8..0x404407`, a 32-bit product of at most 236 106 000 at the level cap 13 107, below 2³¹), written once to `[order+0x3A]` as the capture starts | `edx` = `[order+0x16]`, the TARGET; `edi` the cost; `eax` = 10 + L at `0x4043EC` |
| `0x43869D` (7, `mov cx,[eax+0xB8]`) | `0x438650`, a UNIT RECLAIM's step: `workertime (def +0x1FE) · (k + 5)/5 · the target's maxHP (def +0x1FA) · 15` over the target's cost, `max(def +0x18A, 10.0 at 0x4FD2A4) · 300.0 at 0x4FD2A8` (`0x43865F..0x438680`, `0x4386D2`) (`0x4386B9..0x4386D8`: the four factors multiplied in 32 bits (`0x4386B9 0x4386BC 0x4386C3`), the product read unsigned by `fild qword` at `0x4386CC` with a zero high dword (zeroed at `0x438686`), so it wraps past 2³² − 1, at a factor of 32 for ARMCOM's 300 on a CORKROG, stock's factor at 155 kills: MEASURED 2026-09-26, a step of 1 where the formula gives 486; the quotient cannot reach 2³¹ in stock, whose product is below 2³² and divisor at least 3 000, but a wider product's could, and `_ftol` `0x4E43A0` (`fistp qword` under chop) returns only the low dword: wrong, and made 1 by `cmp eax,1; jg` at `0x4386DF` when negative). **B10 patches it** (sim-fixes.md, B10): `0x4386B9..0x4386CF` computes the product in 64 bits, saturated at `INT64_MAX`, and `0x4386D8..0x4386DE` clamps the quotient to 2³¹ − 1 before `_ftol`, the stubs going on at `0x4386D0` (the `fxch`) and `0x4386DF`, the function's exit at `0x4386E9`; stock's step for every product below 2³². It is the only product that takes a def's `+0x1FA` [DISASSEMBLED, a scan of every multiply: `0x46A4CC`'s `mul` by `0xAAAAAAAB` is the health bar's divide by 3]; the other ten reads of a def's workertime (`0x4024F0 0x4029E6 0x403E1D 0x404111 0x404FBF 0x4055EF 0x405814 0x41420A 0x414630 0x41511A`) divide it by 30 (`0x88888889`, `sar 4`), and `0x42B673` copies it into another def with its neighbouring words. The step goes into `[order+0x36]` (`0x404842`, `0x414C8E`), the HP the reclaim order deals the target as a kind-5 hit every 15 ticks (`0x40496E..0x404986`, `[order+0x3A]` counting the ticks). Its two callers, both `push 0xF` (the ticks), are the reclaim order (`0x40483D`; "Reclaiming" `0x501388`, "That unit cannot be reclaimed" `0x50164C`) and the build order's reclaim (`0x414C86`) — not capture, which an earlier survey had it as | `eax` the RECLAIMER, `dx` its workertime (`0x438694`); `edx` = the factor at `0x4386B9` |

**MEASURED on the new build** (C3's fixture: `VT_LAS` deals 100 to every type, the victims are
unarmed Krogoth clones with 29 918 HP): a keyed shooter at 10 kills and level 10 took 160 a hit,
the unkeyed control at stock's level 2 took 112; a keyed victim at level 10 lost 60 a hit, the
control 92. The keyed tower landed 4.95 hits/s, the control 2.29 (2.16 against the formula's 2.2,
13 ticks against 6). A keyed reclaimer's steps were 472 HP, stock's 128 (3.69 against 11/3). A keyed
capture target (cost × 20/10) took 16.4 s, stock's (× 12/10) 9.9 s. The unit reclaim lands as
damage steps through `0x489BB0` every half second, so the target's own taken level applies to them.

### The kill increment and where the counts part on two peers

- **`0x4869CA`**, in the destructor, is the one increment of the kill count `+0xB8`, of the
  victim's last attacker `+0xF0` and behind three gates
  ([its entry](memory.md#0x4866d0-the-unit-destructor-inferred-name-units_destroy), *Who counts a
  kill*).
- **`+0xF0`/`+0xF4` have two writers that matter**: the hit's apply `0x489CE0` (`0x489DBA`,
  `0x489DC0`, only for a hit with an attacker, on the peer that applies it), and the death's
  receive (`0x486778` from the death record's `+7`, `0x486787` from `0x44FE40([rec+3])`).
- **Where the counts part:** every peer counts a kill only when its own copy of the victim reads `+0x104` = 0.0 (`0x4869A7`), and another peer's copy of a newly created unit reads 1.0 until the owner's round robin writes it, 26–37 s after the create (measured for B8), so a unit killed in that window is counted only on its owner's copy of the killer (*The kill count reads this peer's copy of the
  victim*, above). MEASURED 2026-09-26 on two peers by C3: a host tower's own copy stayed at 10
  through two kills of the joiner's scenario-created units while the joiner's copy counted 0 → 2
  (the pre-C3 build; 3 on C3's); the `+0x104` reads of 0.0 there were taken after the kills, not at
  them. Every level-reading effect reads the copy of the peer that runs it, so inside that window a
  veteran's level depends on which peer computes. A stock defect, outside C3; not fixed.

### The hit sender `0x489BB0` and the HP word

- Stack at `0x489BF3` (after `sub esp,0xC` and three pushes): the caller's amount at `[esp+0x24]`,
  `esi` the victim, `edi` the amount after armour, `ebx` the kind. Kind `0xA` (heal) skips armour
  and veterancy (`0x489BBD` → `0x489C36`, `edx` = the amount).
- **Armour** (`0x489BC3..0x489BF1`): only when the victim's `+0x10E` bit 1 is set, and never for an
  amount of 30 000 or more (`0x489BD1`, `cmp edi,0x7530; jge`): `amount · def+0x1AA / 16` through
  `0x4E43D0`.
- **The store**: `mov [esp+0x11],dx` at `0x489C71` — the 9-byte record's `+5` is a WORD. The apply
  `0x489CE0` subtracts it from the HP word (`sub word [esi+0x108],ax`, `0x489EB5`) and tests the
  result signed (`0x489EBC..0x489EC4`); the paralyser (kind 2, `0x489DFD`) and the heal (kind `0xA`,
  `0x489D6E`) read it with `movzx`. So an amount past 32 767 wraps into a gain.
- **The 30 000 callers** (kill outright): self-destruct and player defeat `0x402147` and `0x486F94`
  (`push 0x7530`, kind 3), a dying transport's cargo `0x48680B` (kind 3 or 6:
  `0x4867DA..0x4867EC` pick 3 when the transport's death byte's high nibble is 3, else 6), and `0x402701`,
  `0x41BC49`, `0x4886A4`, `0x4887D0` (kinds 4 and 9). The disintegrators (`ARM_DISINTEGRATOR`,
  `CORE_DISINTEGRATOR`, default damage 30 000 in `ccdata`, `btdata` and `rev31`) are the only retail
  weapons past 20 000, and their hit is raised by the shooter's level at `0x499DB5`.
- **MEASURED, the pre-B7 build:** `WK_HUGE` (65 000, edge effectiveness 1) left a 1 329-HP storage
  536 HP up a hit (1 329 → 7 225 in eleven hits); a commander with 25 kills (level 5) firing the
  retail D-gun (39 000 a hit) left a storage at HP −11 135 = 1 329 + 2 · 26 536 − 65 536, two
  wrapped hits, dying only because the second wrapped the HP word itself; a Krogoth with 25 kills
  survived its own self-destruct at 5 918 HP (29 918 − 24 000).

### `0x499CD0`, the damage function, and its factor

`0x499CD0(proj, unit, factor)` reads the weapon's default damage zero-extended (`0x499CE3`, the
word `+0xD4`), a per-type entry if the weapon has a table (`+0x64`, a sorted search through
`0x4F8A70`), multiplies by the float `factor` (`0x499D73`), applies the shooter's level
(`0x499DB5`), then the cheat bits of `main+0x37F2F` (bit 7 doubles, bit 8 halves), and calls
`0x489BB0` at `0x499E37`. Its three callers: `0x499C82` in `0x499C70` (factor 1.0; no direct caller,
reached through a pointer), the direct hit `0x49A062` (1.0), and the area pass `0x49A3F5`, whose
factor is `edge + (1 − edge) · (1 − d/aoe)` (`0x49A3B6..0x49A3E0`, `edge` the weapon's `+0xD8`;
1.0 at `d = 0`). So a weapon with edge effectiveness 1 deals every area hit whole.

### The radar rebuild's projectile markers — `0x467300..0x467406`

For a targetable or interceptor projectile (`ebx` = `proj+0x0A`), `al` = the local player
`main+0x2A43`, the sight test is one of two grids by `main+0x14281` bit 1 (a byte grid at the
player record's `+0x7C`, or the bit mask `1 << player` in the word grid `main+0x14273`), `edx` = in
sight. Out of sight (`0x4673AF`), **`0x4673B1` reads the attacker's owner**: `mov ecx,[ebx+0x48]`
(`proj+0x52`), `cmp [ecx+0xFF],al`, the marker drawn only for the local player's own. In sight
(`0x4673BC`), the colour comes from the projectile owner's record: `[ebx+0x5C]` = `proj+0x66`,
`main+0x1B8A + 331 · owner` → `+0x96`.
- **MEASURED, the pre-B7 build:** targetable hail (`WK Hail T`, True line of sight, unmapped)
  faults at `0x4673B4`, "Illegal read, data address 0x000000FF", `ECX=0`. Mapped with Permanent
  line of sight nothing is ever out of sight and the read never runs.
- **Owner 10, the stones' owner, is a populated record:** `main+0x2851` holds the name
  "Player 10…" at `+0x2C` and a non-NULL colour pointer at `+0x27` (`main+0x2878`) [MEASURED], so
  the in-sight path is sound for a stone.

### The meteor shower on two peers

- **The shower** `0x437DE0` runs from the sim tick `0x495594` on every peer, with no host test; the
  map's OTA arms it through `0x437D40` at `0x4367F9`. Each stone is spawned at `0x49DF7D` (a call of
  the projectile init `0x49C740` with no attacker) and broadcast as a `0x0D` (`0x49DFF6`); the
  receiver `0x49D270` (from the dispatcher at `0x455433`) builds it again in its meteor branch,
  `0x49D307`, also with no attacker.
- **`0x49C740(proj, weapon, start, target, time, attacker)`**, stdcall (`ret 0x18`), has eight
  callers; only those two pass no attacker (argument 6, `[esp+0x28]` at `0x49C7A8`; argument 4 is a
  target position, copied to `+0x28` when not NULL, and `0x49CE4A` and `0x49D064` pass it NULL). It
  writes `+0x62`, the firing piece, which the shooter's `QueryPrimary`/`QuerySecondary`/`QueryTertiary` script answers (`0x43E1E0`, run with the slot index), only for a projectile with an
  attacker (`0x49C833`); the one reader of `+0x62` is `0x49B7D6`, behind the burst count `+0x60`,
  which is 0 for a stone. `+0x66` is the owner byte, 10 for no attacker.
- **The damage gate** `0x49A01B..0x49A047` computes a projectile's damage only when its owner's
  record `main+0x1B63 + 331 · [proj+0x66]` is empty or not remote (`+0x73 == 3`), and sends a
  remote one's to `0x49A0AE`: the firer's peer computes a shot. No peer marks record 10 remote.
- **The pool** (base `*(main+0x141F7)`, count `*(main+0x141F3)`, stride `0x6B`) compacts by copying
  records (`rep movs`, `0x49AEE0`), so a record's own bytes travel with it; only projectile code
  reads the pool, and no save carries projectiles.
- **MEASURED, the pre-B7 build, two peers on `WK Hail C`:** every one of 19 stone hits was
  computed on both peers (matched by victim and game time within 20 ticks), and each owner's
  storages lost exactly twice what the stones dealt (38 = 2 · 19, 22, 4 on the host; 10, 36, 30 on
  the joiner). Each peer spawns its own shower: about 2 900 stones a peer in three minutes, each
  received by the other.

### The two kill lines

- **`0x46AEE0`**, the unit panel, reached by `je 0x46AEE0` at `0x46ADD2`: the kill line is drawn
  only when the unit's `+0x110` bit 31 is set and its kills are not 0 (`0x46B2B8..0x46B2D0`); then
  `"kills"` `0x507548` and `"kill"` `0x507540` are translated (`0x4C5740`) and `0x46B306` reads the
  kills. MEASURED: bit 31 is set on armed units once they have fired (the AI's firing towers read
  `0xA0284101` and `0xB0280121`) and never on the unarmed [INFERRED meaning].
- **`0x467CB0`**, a second panel with the same line at `0x467CCF`; no direct caller (TADR's
  `devDrawKills`). Not seen drawn in play.

### The keys' weapons and their sync

- **`0x49E5B0(name)`**, stdcall: the first weapon record whose name equals `name` (`0x4F8A70`),
  over the 256 records of `0x115` bytes from `main+0x2CF3` (A′3 re-bases it, `0x49E5CB` and
  `0x49E5EB`); NULL for a NULL or empty name or none found. The FBI loader resolves ExplodeAs and
  SelfDestructAs with it at `0x42CEA8` and `0x42CEE1`, each value read by `0x4C48C0` into a
  0x80-byte buffer (`0x42CE9B`, `0x42CED4`), and stores them at `def+0x220` and `+0x224`
  (`0x42CEB7`, `0x42CEEC`); a name it does not find gets the stock array's first record
  (`esi` = `main+0x2CF3` since `0x42CDE8`; `0x42CEB1`, `0x42CEEA`).
- **The menu-time loader's folds.** `0x42A8D0` XORs into `CRC_weapons` (def `+0x146`) the stored CRC
  (`[section+0x25]`) of each weapon section the unit's weapon keys, ExplodeAs (key `0x5037F4`,
  `0x42AF23`) and SelfDestructAs (`0x5037E4`, `0x42AF99`) name, each found by rewinding the weapon-TDF contexts (`*0x5122A0`,
  `*0x5122A4` of them, 12 bytes each) with `0x4C3E10` and asking `0x4C3410` for the section
  (`0x42AFC6..0x42AFFD`); the key is read with `0x4C4630` on the unit's UNITINFO context
  (`ecx`, reloaded at `0x42B00A`). `0x42B019` stores the result, with the three arguments of the
  float read at `0x42B01F` already pushed. **A type whose ExplodeAs and SelfDestructAs name the same
  weapon folds it twice: it cancels.** MEASURED: an unarmed ARMCOM clone (COMMANDER_BLAST both)
  reads `CRC_weapons` 0. `+0x146` reaches the lobby's sync value `+0x142` through `0x42A610` (*The
  unit sync's keys*, above). The def array is sealed read-only at `0x42B328`, after the store.

### The effects: the four per-frame arrays [VERIFIED 2026-09-12, landing 4a, objdump of the pristine build]

All four are SIM state: the tick moves them, and the engine's own draw passes (`0x49BE60`
projectiles, `0x420B00` explosions, `0x471F90` the particle layers) read them and write nothing.
That is what lets the frame packet's publisher gather them ONCE PER TICK and reuse the tables for
every publish of that tick.

| array | where | how many, and who says so |
| --- | --- | --- |
| projectiles | count `main+0x141F3`, base `main+0x141F7`, stride `0x6B` | **exactly 300 in stock, 3000 under the raised limits** (*The raised effect pools*, below). `0x499A30` allocates `0x7D64` bytes = 300 × `0x6B` (and `rep stos` clears `0x1F59` dwords, the same 32 100 bytes), then zeroes the count; `0x499A80` frees the base AND NULLS it inside the teardown cascade. **All ten append sites refuse past the cap** — each a `cmp …,0x12C / jge` past the store (`0x49B6EE`, `0x49B809` and eight more; the list is under *The raised effect pools*) — so the count is bounded by the allocation itself and no sanity cap is needed |
| explosions | stock: count `main+0x1491B`, records **inline** at `main+0x1491F`, stride `0x54`; raised: the same layout in a DLL static | **300 in stock, 3000 raised.** `0x420A30`'s add site: `cmp ecx,0x12C / jge` refuses, then `lea eax,[ecx*8+0]; sub eax,ecx; lea edx,[eax+eax*2]; lea esi,[edi+edx*4+4]` — 84 × index past the count word, which is the stride and the base together. Nothing to free: the records are in the block |
| flying debris | stock: the 100 dwords at `0x511DF0..0x511F80`; raised: 1000 dwords in a DLL static. Each names a system whose `+0x2C` is the piece `{node @0, turn words @0x10/0x12/0x14, x @0x16, alt @0x1A, y @0x1E, rotated vertices @0x22}` — the words reach `0x4B6CC0` reversed, as `{w14, w12, w10}` (*The effects models*, below) | the slot count is the address range |
| the ten particle layers | `*(main+0x38D77)`, `0x10` per layer: `{u8 flag, begin @4, end @8, cap @0xC}`. `0x471D90` allocates the table from the level load; `0x471DE0` frees AND NULLS it in the teardown | **The cap plus one: 401 in stock, 20 481 under the raised limits** (*The raised effect pools*, below). Every emitter reads the layer's size and `cmp e?x,0x190 / jbe append`: at the cap or fewer it appends, and **past it destroys the FRONT object, shifts the vector down by one and appends anyway** (`0x472078..0x4720AF`). So the cap plus one is the steady state. **Twenty sites**, and the whole list because a partial one invites the same mistake twice: `0x471183`, `0x4713D8`, `0x471508`, `0x47163D`, `0x471782`, `0x4718B1`, `0x471AD7`, `0x472071`, `0x47219F`, `0x4722CF`, `0x4723D6`, `0x4724D5`, `0x4725D4`, `0x4726C0`, `0x4727B0`, `0x47289A`, `0x47297A`, `0x472A5A`, `0x472BF2` (against `ecx`), `0x472CD9`. The sub-particle vectors inside each object are grown by `0x4732E0` and are the one thing the level fence never covered |

**"The draw passes only read them" is true of the arrays, not of what their fullness decides.**
Two of the caps above gate draws from the simulation's random numbers; see *The raised effect
pools* below.

**MEASURED 2026-10-05, native stock cosmetic pools and fresh seek.** The unchanged
`stock-secondary-orders` fixture self-destructs its selected ARMSOLAR through native Ctrl+D.
At T451, its live and ordinary-playback frame dumps both have explosion count 15 at
`main+0x1491B`; fresh seek has count zero. At T541 live/playback both have count 2. The paused
live twin at T589 has count zero but retains 433 nonzero bytes in the inline record storage.
The strict unlisted-image gate reports 514 failing bytes in the explosion, debris and
flying-piece pools; a focused fresh-worker rerun at the same checkpoints reports 469 in
those same pools. All ordinary state/order/effect-call and restore checks pass.
`tagpu_replay.c:seek_run` calls `tagpu_effects_clear` before restoring recorded tables.
In the native build that function clears the count, reinitialises 300 debris records and
zeros 100 flying-piece slots. In the raised build it clears its corresponding DLL pools;
`tagpu_simreg_recorded` excludes `SIMD_ENGINE` declarations until their layouts exist.
Those routines are unchanged from this landing's base. The bounded native storage is
`main+0x1491B..+0x1AB8F` (count plus 300 × 0x54, 25204 bytes),
`main+0x1AB9F..+0x1E88F` (300 × 0x34 debris records, 15600 bytes), and
`0x511DF0..0x511F80` (100 pointer slots, 400 bytes); upper endpoints are exclusive.
These measurements identify an existing cosmetic-pool snapshot gap. They do not establish
that pool fullness is irrelevant to simulation. The owner approved exactly these three ranges
on 2026-10-06 as a known-gap exception for the native `stock-secondary-orders` unlisted-image
check alone. Every other image bit, fixture and state/order/effect-call/restore check retains
its existing policy. Snapshot layouts for these pools remain unimplemented; see
[recorder completeness](../tadr-port/demo-recorder-completeness.html).

The subsequent whole run samples this control at T301: the image gate accepts zero cosmetic
bytes and also passes without the option. Earlier T451 dumps still fail the default comparator
and pass only with the approved option. The full 37-fixture gate passes with 11/11 changed
function/region requirements reached; these outcomes do not fill the pool-layout gap.

**THE ENGINE'S OWN EXPLOSION DRAW EMITS PARTICLES**, which is not what a draw pass is supposed
to do and is why it is recorded here. `0x420B00`'s debris loop calls `0x421550` at `0x420B18`, and
that function calls the grey-smoke emitter `0x472810` (`0x421583`) and the fire emitter `0x472AB0`
(`0x4215AA`) — both **append to a particle layer**, and past its cap (400 in stock, 20 480 raised)
destroy its front object. So the
ten layers are not constant within a sim tick: a second draw of one tick can find a layer the first
draw did not produce. [ESTABLISHED 2026-09-12 by landing 4a's review, which is what corrected the
frame packet's per-tick cache argument — see the note in `tagpu_packet_pub.c`.]

**Two of the three sequence tables are SESSION assets and the third is PER-LEVEL**, and the
difference decides which of them may cross a thread boundary on its own lifetime and which needs
the fence.

**The effect GAF sequences are a SESSION asset, not a per-level one** [VERIFIED 2026-09-12].
`0x429870` loads the `"fx"` bank through cache accessor `0x429700` and
resolves named sequences into individual fields — `"smoke 1"` → `+0x147CF`, the five
rendertype-4 sprite sequences → `+0x147BB..+0x147CB`, the rendertype-5 flare → `+0x147F3`, the
projectile ground-shadow blob → `+0x1480F`. **Its only caller is `0x49134D`**, inside `0x491200`,
**whose only caller is `0x49EA62`** inside `0x49E830` — WinMain, reached from the SEH wrapper
`0x49EDC0` with the four `WinMain` arguments. So those sequences are loaded once per process. This
is the lifetime argument for a sub-particle's or a projectile's sequence pointer crossing the
thread boundary at all; the ANIM STATES an explosion carries are a different question and are not
established here.

**An EXPLOSION's two anim states do not come from that bank** [VERIFIED 2026-09-12, landing 4a's
review]. The add site takes the sequence from `main+0x1AB8F[idx]` (`0x420AA2`), a table `0x420620`
builds from the level load (`0x4919D2`) and `0x420960` frees and NULLS from the teardown cascade
(`0x491B9F`). So an explosion's frame — and the LHT flash's — is a **per-LEVEL** GAF frame, and
anything holding one across a teardown stands on `tagpu_reclaim`'s fence and on nothing else. A
cache keyed on such a frame's ADDRESS must also drop at a level boundary, because the next level's
allocator can hand a new frame the address an old one had; the effects atlas does that on the
packet's level generation.

`TAProgram` (`[0x51FBD0]`) carries two more tables beside them: `+0xC8` the 32 × 256 LHT
"lighten" ramp the explosion flash is derived from, which the effects pass needs, and `+0xCC` the
256-byte palette remap `0x4BFE10` applies to the fog band, which no pass of ours reads (the grey
band is computed in RGB); and `+0xF0` the capability word (bit 5 the ALP alpha table is
built, **bit 6 the PALETTE.SHD darken table**, bit 7 the LHT one — the three in-place setters
above are what establish the mapping).

### The raised effect pools — every site `tagpu_limits.h` rewrites [DISASSEMBLED 2026-09-23, objdump of the pristine build; MEASURED 2026-09-23]

The four pools above are raised tenfold by the limits block of `tagpu_patches.c`, re-derived from
TADR's `EngineLimits.cpp` (prior art, [the TADR port](../tadr-port/raised-limits.md)), and the
particle layers, their object pool and the composite scratch frame with them, from its
`LimitCrack.cpp`. They share **one table**
with the unit limit's sites (*The per-player unit cap*), the wreck pool's (*The wreck pool, and a
feature paid for and left standing*), the unit-type slots' and the weapon IDs' below, 130 sites in all, compared with the stock
bytes as a whole at `DLL_PROCESS_ATTACH` and written as a whole or not at all; a mismatch writes
nothing and the first DirectDraw call shows the startup-failure report and exits. Every site below
was read out of the pristine image, and the evidence for each is in
[the evidence pass](../tadr-port/limits-evidence.md) §1–4 and §7–10. Landing reviewers checked the sites
against the image byte for byte.

**Projectiles, 300 → 3000.** The pool stays the engine's, allocated per game:

- `0x499A32` the `push 0x7D64` (the bytes, `300 × 0x6B`) and `0x499A56` the `mov ecx,0x1F59` of the
  clear (the dwords), both scaled by ten.
- The ten cap operands of the `cmp …,0x12C` that refuse a new projectile: `0x49B6F0`, `0x49B80A`,
  `0x49C9D2`, `0x49CC34`, `0x49CDF3`, `0x49D011`, `0x49D2BE`, `0x49D4B5`, `0x49DD96`, `0x49DF24`.
  `0x49D2BE` and `0x49D4B5` are inside the `WEAPON_FIRED` receiver, so the cap also refuses a
  projectile a peer fired. `0x49F442`, the eleventh `0x12C` nearby, is a command-line clamp into
  `main+0x37F31` and is not a projectile cap.
- **The compaction `0x49AE20`** keeps two i16 index arrays on its stack, one entry a slot:
  `[esp+0x20+i·2]` and `[esp+0x278+i·2]` after its four pushes. Its `sub esp,0x4C0` becomes a `jmp`
  to a stub that commits the new frame, `0x20 − 0x10 + 4 · 3000 = 0x2EF0` bytes, a page at a time
  (`sub esp,0x1000 / test [esp],eax`, twice, then the remainder) and jumps back to `0x49AE26`. The
  second array's displacement `0x278` at `0x49AEB8` (write) and `0x49AF39` (read) becomes
  `0x20 + 2 · 3000`, and the release `add esp,0x4C0` at `0x49AF7F` becomes `0x2EF0`. Its only
  callers are `0x49BE53` and `0x49C8F4`, both on the game thread. It also fixes up `main+0x142F7`,
  a pointer into the pool.

**Explosions, 300 → 3000, moved into a DLL static.** The engine's inline block at `main+0x1491B`
stays where it is and nothing reads it any more. The layout is kept, `{i32 count; 0x54 × N
records}`, because the add site `0x420A3C` takes the count's address as its base and reaches the
records at `+4`.

- The seven base references, the only ones in `.text`: `0x420630` the reset inside `0x420620`
  (called only from the level load `0x4919D2`), `0x420A36` / `0x420A3C` the add `0x420A30`,
  `0x420B35` / `0x420B3B` inside the draw `0x420B00`, `0x420F66` inside the tick `0x420F30`, and
  `0x421738` in the piece explosion `0x421700`. Each `mov`/`lea` relative to `main` becomes an absolute
  `mov` of the static's address, same length.
- **The sequence table `main+0x1AB8F` stays in the engine.** `0x420AA2` reads it as
  `[edi+eax*4+0x6274]` only because stock's pool sits in front of it (`4 + 300 · 0x54 = 0x6274`);
  with `edi` now the static, that load becomes a `call` to a stub that loads `main` and reads
  `[main+eax*4+0x1AB8F]`. Only `eax` changes.
- The caps `0x420A44` (the add) and `0x421771` (the piece explosion).
- **The tick's compaction is quadratic.** `0x4210E6..0x42113E` finds the first dead record
  (`+0`, `+0xC` and `+0x18` all zero), shifts the whole tail down one record (`rep movsd`, `0x15`
  dwords), decrements the count and rescans from record 0 until a pass removes nothing: O(dead ×
  live) record moves a tick. At stock's 300 that is small; at 3000, with a mass death's explosions
  expiring on the same tick, it is about a hundred times stock's worst. Unchanged by the raise,
  and TADR's too.

**Flying pieces, 100 → 1000, moved into a DLL static.** Seven base operands (`0x420B08`,
`0x420F38`, `0x421153`, `0x421172`, `0x4211A7`, `0x42165F`, `0x421680`) and six end operands
(`0x420B28`, `0x420F53`, `0x421162`, `0x42118D`, `0x4211C3`, `0x42166D`) cover every array use of
`0x511DF0` and `0x511F80`. **The five `mov ecx,0x511F80` stay** (`0x4205F0`, `0x420610`,
`0x420900`, `0x420977`, `0x421691`): `0x511F80` is also the pool allocator that backs the pieces,
the `this` of `0x4379B0` (init) and `0x437A30` (alloc), and its backing grows tenfold at the
`push 0x186A0` at `0x4208FB`. **`0x437A30` is a ring:** an allocation that reaches the end wraps,
and every block it then overwrites is evicted, its owner's slot nulled (`0x437A9D`, `0x437ACF`).
Stock can therefore evict pieces with fewer than 100 alive, and the tenfold ring evicts them later:
below the stock caps the raised build keeps pieces stock would have dropped, and they land and add
their explosions. That changes the explosion count and the C-runtime `rand` stream, not the
simulation's generator unless the explosion pool fills. The level load's clear at `0x42090A` (`mov ecx,0x64 / xor eax,eax /
mov edi,0x511DF0 / rep stosd`, 14 bytes) becomes a `call` to a C reset that clears the static,
returns 0 in `eax` as stock leaves it, and initialises the debris records below.

**Debris records, 300 → 3000, moved into DLL statics.** Stock keeps 300 × `0x34` at `main+0x1AB9F`,
each owning 8 vertices of 12 bytes at `main+0x1E88F + i·0x60` and 6 face records of `0x20` at
`main+0x2590F + i·0xC0`. The level load initialises them in two loops: `0x4207FB..0x42088F`
writes each record (`+0` = `0xFF` free, `+4` = 8, `+8` = 6, `+0xC` = −1, `+0x1C` = `0x502C28`
from `edi`, set at `0x4207DD`, `+0x20` = 0, `+0x24` the vertices, `+0x28` the faces), and
`0x420895..0x4208F9` its faces (`+0x1C |= 1`, `+0` = `0xC8`, `+4` = 4, `+0xC` = `0x502BF8 + k·8`
for the six faces). The C reset writes the same values into the statics; the stock loops still run
over the engine's array, which nothing reads. Two first-free scans hand out a record, both
`cmp eax,0x12C`: the standalone allocator `0x420920`, which nothing in the image calls or points
to, becomes a `jmp` to the C allocator so that no copy of the stock scan survives, and the inline copy
at `0x4217DE` (16 bytes: `mov edx,[main] / xor esi,esi / xor eax,eax / lea ecx,[edx+0x1AB9F]`)
becomes `xor esi,esi` / `call alloc` at `0x4217E0` / `jmp 0x421804` at `0x4217E5` and four NOPs. `0x421804` wants `eax` = the
record or 0 and `esi` = 0, and reloads `ecx` and `edx` before it uses them. **The C allocator is
first-free, as stock's is**, so below 300 it hands out exactly the records stock does, and nothing
carries from one game to the next.

**The caps gate the simulation's random numbers.** `0x421700`, the piece explosion, walks a dying
unit's pieces; for each it leaves when the explosion count reaches the cap (`0x42176F`); when no debris record is free, `0x421802..0x421808` skips the draws that
follow. Otherwise it **draws the sim RNG `0x4B6C30` eight times a piece**: six at
`0x421851..0x4218D2` and two at `0x421B39` and `0x421B62`, on the same path with no branch
between. So the explosion cap and the debris records' fullness decide how many sim-RNG draws a
death makes. Below the stock caps the raised build makes the same draws in the same order; past
them, the raised build draws where stock would have stopped.

**The flying-piece cap gates no draw.** The live spawner is `0x481140`, reached through the
function-pointer table entry at `0x4FD6CC` (`.rdata`, among `0x480xxx`/`0x481xxx` handlers), not
by a call. It draws the sim RNG six times (`0x48116F..0x4811B9`) and only then asks
`0x421620` for a slot (`0x48123C`). `0x421620` hands a piece flagged to explode (record `+0x28`
bit 2) to `0x421700` before it scans for a slot, so that branch does not depend on the slots
either. `0x420E50`, which has the same shape and also calls `0x421620`, has no caller or pointer
anywhere in the image; nor do `0x421150`, `0x421170` and `0x4211A0`, three slot helpers whose
operands are patched with the rest.

**Particles, 400 → 20 480 a layer and 1000 → 204 800 objects.** Two ceilings, both visual: the
emitter range `0x470F00..0x472F00` draws the C runtime's `rand` (`0x4E4870`) and never the
simulation's generator.

- **The layer cap is twenty operands**, every `0x190` compare in the emitters: nineteen
  `cmp eax,0x190` (`3D imm32`, operand at +1) at `0x471183`, `0x4713D8`, `0x471508`, `0x47163D`,
  `0x471782`, `0x4718B1`, `0x471AD7`, `0x472071`, `0x47219F`, `0x4722CF`, `0x4723D6`, `0x4724D5`,
  `0x4725D4`, `0x4726C0`, `0x4727B0`, `0x47289A`, `0x47297A`, `0x472A5A`, `0x472CD9`, and
  `cmp ecx,0x190` (`81 F9 imm32`, operand at +2) at `0x472BF2`. Past the cap the emitter destroys
  the layer's front object and shifts the rest down (`0x472078..0x4720AF`), so a layer holds the
  cap plus one.
- **The pool `0x51E610` is built once, by the C runtime's static initializer** `0x471C80`:
  `push 0x4C; push 0x3E8` (operand `0x471C83`), `call 0x470A90`, then `atexit(0x471CA0)`. The
  constructor calls `0x470C10` once (its only caller, `0x470ACB`), which `realloc`s the pointer
  stack at `+0x14` to 4 × capacity and `malloc`s capacity × `0x4C` bytes in one block; nothing grows
  it later. The alloc `0x470EB0` is `used < capacity ? stack[used++] : 0` (`+0x20` used, `+0x1C`
  capacity), the free `0x470ED0` is `stack[--used] = p`. All 28 references to `0x51E610` are the
  twenty allocs, six frees, the constructor and the `atexit`. An empty pool makes the emitter skip
  the particle (`0x471FFC..0x47202E`). DllMain runs before that initializer, so writing the operand
  is enough; TADR hooks `0x471C87` and rewrites the pushed value instead. At 204 800 the block is
  15.6 MB and the stack 0.8 MB, once a process.
- **Eight of the twenty compares are in code nothing reaches.** The emitters at `0x471160`,
  `0x471340`, `0x471470`, `0x4715A0`, `0x4716E0`, `0x471820`, `0x471A50` and `0x472720` (the compares
  `0x471183` … `0x471AD7` and `0x4727B0`) have no call, jump or pointer anywhere in the image, nor
  does `0x471D10` with its alloc; the twelve live emitters are `0x471FD0`, `0x4720D0`, `0x472200`,
  `0x472330`, `0x472430`, `0x472530`, `0x472630`, `0x472810`, `0x4728F0`, `0x4729D0`, `0x472AB0`
  and `0x472C50`. The dead sites are written with the rest, since their bytes are compared anyway.
- **The layers' tick `0x471EB0` erases by shifting:** for every object of every layer it calls
  `vtbl+0xC`, and a finished object is deleted and the tail moved down one slot. Like the emitter's
  own front-drop, that is O(layer) a removal; it runs in the sim tick (`0x4955BF`). The teardown
  `0x471DE0` empties a layer front first the same way (`0x471E17..0x471E48`), n²/2 moves a layer.
  The draw is `0x471F90(ctx, layer)`, called ten times from DrawGameScreen (`0x469849` …
  `0x469D2C`), which calls `vtbl+8` on each object; `0x471F40`, a whole-table twin of it, has no
  caller.
- **MEASURED 2026-09-23 in tier 1** (4 × 1500 units, the scratch build first, then the landing's):
  one layer, layer 9, peaked at **13 529 objects** at the opening volley (its objects' vtable
  `0x4FD618`, dark smoke; the rocket kbots' trails <span class="pill pill-warn">INFERRED</span>),
  the pool's used count at **13 571**, and the frame packet's sub-particles at **14 510** and
  **15 964** in two runs. The simulation held 60 ticks a second throughout. Stock would have held
  that layer to 401.

**The composite scratch frame, 600 × 600 → 1280 × 1280** [DISASSEMBLED 2026-09-23; MEASURED
2026-09-23]. One frame a level: the unit blit's work frame (the build-state copy and the shadow
passes draw there, and the GDI lane shows what they leave) and the 2× bake's canvas.

- **Where it lives.** The composite draw context `*(main+0x1437B)` is a `0x14`-byte object the
  model loader makes once a level (`0x4B4F10(0x14)` at `0x42D3C0`, the constructor `0x458160`,
  stored at `0x42D3DD`). `0x458180`, called at `0x42D473` with a page-rounded size, allocates the
  context's ring at `+4` (`0x4379B0`: frees any old one, `[+0]` the size) and then the scratch at
  `+0x10`: `push 0x258` (the height, operand `0x458196`), `push 0x258` (the width, operand
  `0x45819B`), `push 0x506604`, `call 0x4B8E00`. It is the only such pair in `.text`.
  `0x4B8E00(name, w, h)` allocates `w·h·2 + 0x18` bytes: the header (`+0` width and `+2` height,
  u16), the colour plane at `+0x18` (pointer at `+0x10`) and the depth plane right after it
  (`+0x14`). The level teardown frees it inside `0x42DB90`: `0x42DC8F` calls `0x4581C0`
  (`MEM_Free [+0x10]`, then `0x437A20` frees the ring), `0x4B4F20` frees the context and
  `0x42DCA3` nulls the pointer. Nothing leaks at 1280², 3.28 MB a level.
- **It is not the per-unit composite.** The unit's own frame at `Object3do+0x10` is allocated by
  the builder `0x4586A0` at the AABB's size, through `0x437BE0(slot, w, h)` (colour and depth,
  `0x458719`) or `0x437B50` (colour only, `+0x14 = 0`, `0x458702`), both a block of the context's
  ring: they call the ring allocator `0x437A30` (the flying pieces' too) with `this` unchanged. What
  caps it is the ring, not 600 × 600: `0x437A30` refuses a block larger than the ring
  (`0x437A65..0x437A77`: the slot is nulled and it returns 0), and the builder then skips the unit
  (`0x45871E`). The ring is sized once a level from the screen, `main+0x37E23` (height) ×
  `main+0x37E1F` (width) at `0x42D3E9`, page-rounded at `0x42D466`: 6 242 304 bytes, 13 × the
  pixels, on an 800 × 600 peer [MEASURED 2026-09-24].
- **Four writers size it to a unit, and none compares with the allocation**, all called from the
  blit `0x459200` with the context as `this`. The build-state copy `0x4589C0` (from `0x459608`,
  every frame, for a unit with a depth plane) writes the box of the unit and its cargo into the
  header (`0x458B8C`, `0x458B92`) and copies or clears both planes at that size. The frame copy
  `0x45A470` (from `0x459338`, `0x4594DB`, `0x45958C`, under the Shadow and TShadow bits) copies a
  unit's own frame, header and both planes (`0x45A4C8`, `0x45A4F1`). The shadow build `0x45A790`
  (from `0x4592FE`, `0x45955B`, for an object with no depth plane) takes a box from the model's
  vertex extents (`0x45A510`), writes it into the header, clears both planes (`0x45A80E`,
  `0x45A82D`) and rasterises into them (`0x45A610`). The 2× bake in both rasterisers, `0x459830`
  and `0x459C70` (AntiAlias, `main+0x37F06` bit 1; the structure bit, `unit+0x110` bit 29;
  `mode != 0`), doubles the box (`0x459899` … `0x4598C1`), writes it into the header and clears
  both planes at the doubled size (`0x4598E4`, `0x459908`).
- **So a box larger than the frame's area, or than a quarter of it for the 2× bake, writes past
  it**: 600 × 600 and 300 × 300 in stock, 1280 × 1280 and 640 × 640 raised. The ring's cap does not
  keep a unit frame within the scratch: at 800 × 600 it has room for a frame of 3.1 million pixels
  against the scratch's 1.6 million. **TADR's raise only moves the threshold**; the bound is the engine fix in *Engine defects we
  patch* ([The composite scratch frame and the rasterisers' span
  tables](render.md#the-composite-scratch-frame-and-the-rasterisers-span-tables)), which also bounds the
  rows a writer hands the rasterisers under it. Every
  reader takes the size from the header a writer set earlier in the same call, and the only `0x258`
  immediates in `.text` are the two raised, so the raise lengthens no write.
- **It runs on every lane** [MEASURED 2026-09-23]. After tier 1 on the Vulkan lane the scratch's
  header held a 39 × 42 box with a hotspot of 21, 24, a 1× writer's (the 2× bake writes only even
  values), and its planes were `0x190000` = 1280² apart; the stock-limits build reads `0x57E40` =
  600². On the GDI lane the nanoframe ladder (`scenarios/nanoframe-ladder.json`) looks the same
  on both builds: every pixel that differs lies inside the nanoframes, and stock against raised
  differs by the same 10 733 pixels as two frames of one run a second apart, the build-state pulse.

**Unit-type slots, 512 → 16 384** [DISASSEMBLED 2026-09-24; MEASURED 2026-09-24]. A category mask is
one bit a type: 0x40-byte heap blocks the name map `0x488C50` allocates (`push 0x40; call 0x4B4F10` at
`0x488CC2`) and clears (`0x488CD3`), ORs (`0x488E3E`), sets a type's own bit in (`0x488E03`, and at game
start `0x488E70` from `0x42D6C2`), and 0x40-byte stack masks in the AI plan's `Weight` and `Limit`
commands (`0x406DB0`, `0x406E40`, filled by `0x488D30`) and in Ctrl-Z (`0x48BE00`). Every reader
indexes one by a type ID below the def count, and nothing else sizes them. The 17 sites: the heap
allocation (a stub, since `push 0x40` has no room for 2048), its clear and OR counts, and in each
stack frame the entry, the clear count, the displacements above the mask and the release — the mask
sits at the frame's foot with at most one dword under it, so it grows upward over the frame and only
the displacements above it move (`0x406DB5`, `0x406DBE`, `0x406DC9`, `0x406DFD`, `0x406E3A`; `0x406E45`,
`0x406E52`, `0x406E5D`, `0x406E64`, `0x406EB2`, `0x406ED6`; `0x48BE08`, `0x48BE22`, `0x48BF1E`). A
0x804-byte frame is inside one page, so no probe is needed.

- **The count check**, `0x42AA65` in the menu-time loader `0x42A8D0` (`inc ebx; mov edx,[main]`, ebx
  the files found): a call to a stub that refuses more than 16 384 slots with a dialog and
  `ExitProcess(1)`, before the def array is allocated from the count (`0x42AA8A`). Every later count
  is that array's: `0x42B2F6`, `0x42D542` and `0x42BD02` only shrink or zero it. MEASURED: 16 106
  generated types (16 384 real) show the dialog and exit on OK, with no `ErrorLog.txt`.
- **The join's pace**, `0x46DE91`: the joiner's unit-sync checksums go 64 types a tick instead of 4
  (*The unit sync's keys, and the join's pace*).
- **The engine requires the FBI's `Copyright` key.** `0x42B0E2..0x42B162` compares it with the
  template at `0x5038CC`, the year masked; a mismatch clears def `+0x241` bit 23, and the game load
  then skips the type (`0x42D224`). A generated FBI without the line is counted and never loaded.
- **Which Ctrl-letters read a mask.** The in-game key table (`0x496694` indexes `0x4965F4`): Ctrl+A
  (code `0xAA`) selects everything (`0x4963CE` → `0x48BD50`), Ctrl+C the commander (`0x4963FE`), Ctrl+Z
  every unit of the selected types (`0x496413` → `0x48BE00`), and Ctrl+B and Ctrl+E..Y but S select by
  the category `CTRL_<letter>` (`0x4963D8` formats it and calls `0x48BF30`). Stock units carry the
  categories B, C, F, P, R, V and W.
- **MEASURED**, Core Prime Industrial Area: at IDs 512 and 543, the previous build's Ctrl-Z returns
  to `0x80496419` (a return address with bits set by the overrun) and an AI `Weight` line naming ID
  543 faults at `0x804B79DB` during the load, while ID 512 alone mis-returns silently; this build
  selects every generated unit by Ctrl+G and by Ctrl-Z. At the ceiling, 16 383 real types:
  `UNITINFOCount` reads 16 384, a `Weight` line naming ID 16 383 loads, and Ctrl+G and Ctrl-Z each
  select all 10 generated units.

**Weapon IDs, 256 → 4096** [DISASSEMBLED 2026-09-24; MEASURED 2026-09-24]. `Weapons[]` moves to a
DLL static of 4096 records of 0x115 bytes (1.1 MB, for the life of the process); the block at
`main+0x2CF3` stays and nothing reads it. A weapon's ID is then `(pointer − base) / 0x115`, exact
and bounded, since every pointer the engine keeps to a weapon (unit defs, unit slots, projectiles
`+0x00`, feature defs `+0xE4`, the meteor's `0x512328`) is into the static. 25 sites, the
evidence's complete list of 18 displacements and 3 bounds ([§9](../tadr-port/limits-evidence.md))
reached as follows:

- **Five `mov reg,[0x511DE8]` that feed only a weapon's address** become an immediate of the
  static's base less `0x2CF3`: `0x42CDCD` (the unit loader's `Weapons[0]` for a weapon name it does
  not find, read at `0x42CDE8`), `0x42F3AB` (the release, `0x42F3B3`), `0x49E5CB` (the name lookup,
  `0x49E5D1`), and the meteor's `0x437CF7` and `0x437D13` (`0x437CFD`, `0x437D19`; six bytes, the
  last a NOP). The bounds `cmp r,0x11500` at `0x42F431` (release) and `0x49E5EB` (lookup) become
  `4096 · 0x115`. The loader's base is the both-builds site `0x42E468` (*A weapon's ID, and the
  messages that carry it*).
- **The wipe**, the loop `0x42E31C..0x42E345` inside `0x42E310` (every level load, `0x4918BB`; its
  displacements `0x42E322`, `0x42E32A`, `0x42E332` and bound `0x42E33E`), becomes a jump to a stub
  that empties each slot's name and sets its **ID byte `+0x10A`**: the slot's index below 256, from
  256 its low byte, or `0xFF` where that is 0. The AI's "armed" tests read the byte as `!= 0`
  (`0x40954D`, `0x409682`, `0x409940`, `0x49E0C2`); nothing reads it as an index any more. A saved
  game stores it with each unit's weapon (`0x487A1C`) and a load writes it back into the weapon's
  record (`0x487628`, `mov [edx+0x10A],cl`, `edx` the slot's weapon), so under a changed mod it
  can be stale: every reader of ours takes the index from the pointer.
- **The model path.** Its loop `0x42EC99..0x42ECF7` (`0x42ECCA`) runs to the ID byte, looking for a
  lower slot whose model has this weapon's model name, and on a match branches to
  `0x42F340..0x42F38E` (`0x42F364`, `0x42F380`, `0x42F387`), which only that `je` at `0x42ECE1`
  reaches. It becomes a stub that walks every lower slot and lends the model (`+0x74`, the name at
  `+0x80` emptied), going on at `0x42EDA1`, or loads it at `0x42ECF9`. The store `0x42ED46..0x42ED7A`
  (`0x42ED67`, `0x42ED74`) is rewritten in place to address the record through `ebp`: `mov
  [ebp+0x74],esi; lea edi,[esp+0x40]; lea edx,[ebp+0x80]; jmp 0x42ED7B`. The copy at `0x42ED7B` is
  stock's unbounded one: the name was read into a 0x100-byte buffer (`0x42EC7B`), so a long name
  runs up to `0x6B` bytes past its record into the next, as in stock; the static carries a tail
  of that size after its last record, so no write of the model path leaves the array.
- **`0x0D`, weapon fired.** The five sends, `call 0x451DF0` at `0x49D859`, `0x49DB4D`, `0x49DD27`
  and `0x49DEEE` (a unit's slot, in `esi` or `ebx`, the weapon at its `+0xC`) and `0x49DFF6` (the
  meteor, the weapon its function's argument; its `+0x1A..+0x23` come off the stack uninitialised
  and are cleared), write the ID's low byte at `+0x19` and bits 8..11 into the high nibble of the
  slot byte `+0x23`, which stock's unit senders fill with `and dl,3`. The receiver's block
  `0x49D280` reads them back (`0x49D295`, `0x49D29F`) and keeps the slot byte's low nibble for
  `0x49D366` and the extra-weapons splice there.
- **`0x0F`, feature hit.** The weapon's send `0x424575` carries bits 8..11 in bits 12..15 of the
  cell x; the receiver (the dispatch slot `0x455FB8`, the both-builds site) adds them back.
- **`0x0E`, interceptor detonation.** Area damage `0x49A120` sends two when an interceptor catches a
  projectile: the first (`0x49A769..0x49A7A8`) carries the caught projectile's target point and
  weapon byte, the second (`0x49A7AD..0x49A7E9`) the interceptor's. The two byte reads `0x49A78C`
  and `0x49A7CD` become calls that take the index from the weapon pointer: below 256 the byte, and
  the stock message goes; from 256 up the stub sends the companion itself, a 65-byte `0x05` of
  `05 00 49`, the 12-byte target point and a `u16` ID, through the same net handle (`esi+0x52` →
  `+0x96` → `+4`), and clears the type byte; the sends at `0x49A7A8` and `0x49A7E9` become calls
  that send only a message whose type is still `0x0E`. The receiver `0x49AF90` compares the target point (`+0x28`, `+0x2C`, `+0x30`
  against the message's `+1`, `+5`, `+9`) and then the byte (`0x49AFC9`, 11 bytes, whose `je
  0x49AFE8` detonates the first match): that compare becomes a call that matches a weapon whose
  index is below 256 and equal to the byte, its answer in the zero flag the `je` reads. The dispatch
  table's `0x05` slot `0x455F90` goes to a stub that handles a companion (`05 00 49`) on the game
  thread in play (`main+0x391F5` = `0x499200`, the in-play handler) by detonating the first local
  projectile with that target point and a weapon of that index, through `0x499EB0(proj, 0)`, and
  then continues to stock's `0x45522E`, which returns at once for text that starts with a zero
  byte (`0x463CA7`). The gate is an ordering. During a network load two threads pump messages,
  the loader (`0x49727D`) and the game thread's loading screen (`0x49852E`), while the loader
  allocates the projectile pool (`0x499A30`; freed and nulled at `0x499A9A`). `0x499200` is
  stored in two places: `0x498455`, which the game thread reaches after reading bit 1 of
  `main+0x38D75`, the loader's last store (`0x497C62`), and `0x490BC5`, `SetInputMode`
  `0x490B30`'s mode 6, which none of its eleven callers passes (they pass 1, 2 and 7).
  `0x49847E` and `0x498480` clear the flag word `main+0x38D6F..0x38D76` right after the install,
  so each load waits on its own loader. With the handler there the pool is complete and no other
  thread writes it. The handler stays for the level. Every way out of play replaces it
  (`SetInputMode(7)` at `0x41F668` and `0x427736`, `(1)` at `0x460653`, the post-game
  `0x4996DF`, the stores at `0x49297E`, `0x492A7F` and `0x499852`), after which the gate drops;
  where the teardown runs before the replacement, it nulls the pool at `0x499A9A` on the game
  thread, and the stub's `!pool` test drops.
  `0x497F40`, the loading screen, is stored only by the menu handlers (`0x496C3D`, `0x496D75`,
  `0x496D9D`, `0x496DFF`) and by `SetInputMode`'s mode 5 (`0x490BB3`), never over `0x499200`. The
  residual is the first in-play frame, which `0x49842F` runs (`0x496790`) before `0x498455`: a
  companion that arrives in it is dropped, and logged. A companion dropped before play can name
  only a remote copy, whose detonation is visual: `0x499EB0` damages for a projectile of a local
  owner only.
- **MEASURED**, `tools/weaponids_fixture.py` on Two Continents. Single player: a tower firing
  weapon 4000 hits its target, 31 a hit, through a projectile whose weapon pointer is the static's
  record 4000, and the weapon borrows the model of the stock weapon it copies, drawn in flight; the
  weapons with no `ID=` and with 5000 are skipped with their log lines and their towers stay
  unarmed; the extra-weapons module was installed beside them. Two peers, the joiner firing at the host's
  wrecks and units: the host decoded every feature hit at its full ID, 3581, 3582, 3583, 3322
  and 4000, and told 250's and 3322's hits on one cell apart (both send the byte `0xFA`); the
  host's interceptor, ID 3000, caught 11 of the joiner's rockets, 250 and 3322 aimed at one point,
  and the joiner detonated the right one every time: the 6 of 250 through stock's `0x0E`, the 5 of
  3322 and the 11 interceptors through the companion. Paused once the fire stopped, both peers held
  the same units at the same health, the same features cell for cell, and the same `GameTime`.
- **What a detonation can reach.** Either message detonates only a projectile still in flight on
  the receiver. With the Merl's own speed the host caught the rocket near its target, and the
  companion found no rocket on the joiner 4 times of 4 — [INFERRED] the joiner's own copy, ahead of
  the host's by the link's delay, had already hit, which is stock's window for its `0x0E` as well;
  the test's rockets fly at 120 so that the catch is early. The peers still agreed.

**What it measured, 2026-09-23.** Single player, `scenarios/limits-flood.json` (450 Merls against
450 Diplomats): the engine's own counts peaked at **687 projectiles** and **1727 explosions**, and
the frame packet carried **687 projectiles, 2439 explosions and 540 flying pieces** at its peaks,
with no table truncated. Network game, two peers on the
raised build (`scenarios/limits-mp-west.json` on one, `-east` on the other): projectiles
**689 on both**, explosions **1770** and **984**. Paused with the game's own `Pause` (`main+0x38A51`
= 1 on both, their tick counters three apart), **both peers held the same 394 units at identical
positions**, 0.00 world units apart, matched by engine index, which is the same slot on every
peer. A first round (Hammers against Thuds, 630 units) matched the same way.

**Ten peers at 1500, 2026-09-24** (`scenarios/limits-tier2-p0` … `p9`, one per peer, Town &
Country, network speed 10): `main+0x14351` read **15 001** on all ten and the per-peer counts
peaked at 14 991–15 000 alive. The engine's own counts, polled on every peer for three minutes,
peaked at **731–854 projectiles** (`main+0x141F3`), **2936–2966 explosions** (the moved pool's count
word), and **12 759–14 331 particle objects** in use (`0x51E630`, the pool's used count); `GameTime`
(`main+0x38A47`) advanced 30 a second on every peer. Paused, the ten rosters held the same 7307 units
in the same slots with the same types; 6998 stood at identical positions on all ten, and the
moving rest sat off the owning peer's copy by at most 40 ticks of their own `maxvelocity`: a remote
unit is where its owner last reported it. The pause landed on `GameTime` 6440–6455 across the ten.
**`ActiveCommanderDeath` (`main+0x37EF6`) decides whether a dead commander takes its army with it,
on every peer** (`0x486688` → `UNITS_KillAllForPlayer`); the lobby's default is on, and a run with
it on lost a whole army to one commander's death.

**`unit+0xFF` is the LOCAL player index**, not a global one [MEASURED 2026-09-23]: every peer
lists itself as player 0, so the same unit reads owner 0 on the peer that owns it and owner 1 on
the other. A peer that creates units for another player's slot makes units no peer owns: they
exist on that peer alone. The team colour is the owner's player record, `player+0x96` (see
`0x467C00` above).
