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

   The read-only oracle works armed or not: drop `tagpu_weapons.trigger` next to
   the exe (one engine unit index per line, or `all`) and the module writes
   `tagpu_weapons.json` — every slot of every requested unit (state byte, weapon,
   target, reload, heading, pitch, stock, aim result, COB thread) plus the arming
   state and the C-path hit counters. `tacli weapons <name>` is the CLI half. */

void tagpu_weapons_init(void);                 /* DllMain: arm if the flag file exists */
void tagpu_weapons_frame(unsigned int frame);  /* overlay: oracle trigger, every 5th frame */

#endif
