#ifndef TAGPU_WEAPONS_H
#define TAGPU_WEAPONS_H

/* tagpu_weapons — 1..N weapons per unit (research/notes/extra-weapons.md).

   The engine bakes "a unit has three weapons" into a 2-bit slot index, three
   inline 28-byte slots in UnitStruct, three def pointers in UnitDefStruct and
   about two dozen loops and address computations. This module lifts that to a
   per-unit-type count read from `Weapon4..WeaponN` FBI keys, with a fixed
   capacity (WPN_CAP). Slots 0-2 stay where the engine keeps them; 3..CAP-1
   live in a side table owned by this DLL, keyed by the unit's array index.

   THE FIRST MODULE THAT CHANGES THE SIMULATION. The renderer's rule ("hooks
   read-only over sim") does not apply here, so the module lives behind its own
   gate and never shares state with the render passes.

   OFF BY DEFAULT. Nothing is patched unless `tagpu_weapons.on` exists next to
   the exe at DLL attach. Arming is all-or-nothing: every site's bytes are
   verified before the first write, and one mismatch leaves the whole image
   untouched (`weapons: disarmed (...)` in tagpu.log).

   Stock units run stock code. Every replaced engine function is entered through
   a C wrapper that trampolines to the untouched original for units whose type
   has three or fewer weapons (or for slot indices below 3). The C ports are only
   ever entered for extended units; hit counters prove it (`hits` in the oracle
   dump below, assertion 4 of the note).

   IT ALSO OWNS AIMING AND GROUND ORDERS FOR THE EXTENDED SLOTS. Two engine
   behaviours are hard-coded for three weapons in ways a loop bound cannot reach,
   so the module supplies them instead. (a) The attack-*ground* order sets a
   ground target on slots 0 and 1 only — `FUN_004038A0` unrolls exactly that pair
   — so a splice extends it to the side slots. (b) A unit gets eight COB script
   threads (`0x4B08C0`), and an aim script that waits for its turn holds one for
   the whole slew; four more of those exhaust the pool, whereupon `QueryScript`
   fails *silently* and starved slots aim from piece 0. So the extended slots run
   an aim script that returns inside its own tick, the module re-solves them every
   tick rather than latching the engine's "already aiming" bit, and it withholds
   the shot until the muzzle piece really points at the target. Both are visible
   in the oracle: `ground`, `cob_full` (must stay 0) and `hold_fire`. Slots 0-2
   keep the engine's own behaviour untouched in both cases.

   The read-only oracle works armed or not: drop `tagpu_weapons.trigger` next to
   the exe (one engine unit index per line, or `all`) and the module writes
   `tagpu_weapons.json` — every slot of every requested unit (state byte, weapon,
   target, reload, heading, pitch, stock, aim result, COB thread) plus the arming
   state and the C-path hit counters. `tacli weapons <name>` is the CLI half. */

void tagpu_weapons_init(void);                 /* DllMain: arm if the flag file exists */
void tagpu_weapons_frame(unsigned int frame);  /* overlay: oracle trigger, every 5th frame */

#endif
