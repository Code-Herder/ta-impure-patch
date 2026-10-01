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
   read-only over sim") does not apply here, so the module never shares state
   with the render passes.

   ALWAYS ON, AND FAIL-CLOSED LIKE EVERY OTHER SIMULATION CHANGE. Its sites are
   sites of the fail-closed table (tagpu_patches.c, fix_extra_weapons): compared
   with the exe file together with the rest of the table, written with it or not
   at all, and re-read at the first DirectDraw call; a site that differs ends the
   launch with the table's report. There is no switch, so every build plays the
   same simulation and a recording carries no arming state.

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

   The read-only oracle: drop `tagpu_weapons.trigger` next to the exe (one engine
   unit index per line, or `all`) and the module writes `tagpu_weapons.json` —
   every slot of every requested unit (state byte, weapon, target, reload,
   heading, pitch, stock, aim result, COB thread) plus the C-path hit counters.
   `tacli weapons <name>` is the CLI half. */

/* One site of the module as the fail-closed table takes it: the retail bytes
   the table compares, and the bytes it writes in their place. */
#define TAGPU_WPN_SITES     44    /* 19 entry hooks, 21 splices, 4 byte patches */
#define TAGPU_WPN_SITE_MAX  18    /* the longest site: the splice at 0x49DCA4   */
typedef struct TagpuWpnSite {
    unsigned int         va;
    int                  n;
    const unsigned char* stock;
    unsigned char        ours[TAGPU_WPN_SITE_MAX];
    const char*          name;
} TagpuWpnSite;

/* DllMain, from fix_extra_weapons, before the table is compared: the module's
   memory, the script names, the two record observers (0x42D2E0, 0x42BF40) and
   every trampoline and stub, then each of its sites into out[0..TAGPU_WPN_SITES).
   Writes none of those sites itself. Returns TAGPU_WPN_SITES, or one of the two
   failures below with the reason logged; on either, the table refuses. */
#define TAGPU_WPN_OWN        (-1) /* its memory, a stub, an observer's landing or its site list: our side */
#define TAGPU_WPN_OBSERVERS  (-2) /* an entry the record observers need holds other bytes: the exe's side */
int  tagpu_weapons_sites(TagpuWpnSite* out);

void tagpu_weapons_frame(unsigned int frame);  /* overlay: oracle trigger, every 5th frame */

/* GAME thread, for the weapon keys' detour on 0x49ABB0 (tagpu_patches.c):
   the can-engage verdict of slot idx >= 3. */
int   tagpu_weapons_check_slot(char* unit, char* target, unsigned idx);
/* A slot's weapon pointer for the 0x0D wire check and the weapon keys, or NULL
   when the slot is past the unit's own count. No clamp and no engine write;
   past slot 2 it may (re)build the module's side table through side_row, as
   any side-row read does. */
char* tagpu_weapons_slot_weapon(char* unit, unsigned idx);

/* A seek on a level that plays a recording (tagpu_replay.c, seek_run), on the game thread between
   two sim steps, once the rebuild has written this module's declared tables: re-establishes the
   def records' invariants their layout cannot express. Returns what it repaired. */
unsigned int tagpu_weapons_restored(void);

#endif
