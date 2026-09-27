#ifndef TAGPU_DATAKEYS_H
#define TAGPU_DATAKEYS_H

/* tagpu_datakeys — TADR section C's new data keys
   (research/notes/tadr-port/data-keys.md).

   The unit-key reader: an observer inside the FBI loader 0x42BF40, at
   0x42BF97, reads the keys this module owns out of the FBI section the loader
   is holding, through the engine's own TDF reader, and keeps one record per
   UnitDef slot. Observers at the unit-data load's start (0x42D2E0) and the
   loader's entry keep each record to its own load, and one on the checksum
   0x4B6BA0 records each COB block's length for the mask's bounds. Installed
   at attach for the process, before the fail-closed table, each site
   byte-matched; a mismatch leaves that site untouched, skips the sites that
   rest on it and logs. One at the load's start, the loader's entry or its
   read site also makes the table refuse to install (the veterancy keys the
   reader carries are simulation); one at the checksum only costs the mask.

   The build ghost's piece mask (C1): which pieces a type's ghost hides. By
   default the pieces its COB `Create()` hides before anything else runs; with
   `PreviewPieces=` in the FBI, every piece that list does not name. Computed
   on the game thread, cached per type for the level, published in the frame
   packet as TAGPU_PK_GHOSTMASK. The render thread reads only the packet. */

#include "tagpu_packet.h"

/* The weapon keys (C2): one byte a weapon, indexed by the weapon's validated
   ID, written by the weapon loader on the LOADER thread and read on the GAME
   thread in play. The sites that consult it are in the fail-closed table
   (tagpu_patches.c, fix_weapon_keys) and in the extra-weapons module's C
   paths for slots past 2. A weapon without a key reads 0 and runs stock. */
#define TAGPU_WK_NOTTOAIR          0x01u
#define TAGPU_WK_NOTTOUNDERWATER   0x02u
#define TAGPU_WK_SURFACEFIRE       0x04u
#define TAGPU_WK_NOTOVERWATER      0x08u
#define TAGPU_WK_NOTOVERLAND       0x10u
#define TAGPU_WK_NOMAPALERT        0x20u

unsigned tagpu_datakeys_wkey(const void* weapon);
/* LOADER thread: the weapon load's entry 0x42E310, and A'3's ID site 0x42E468
   with the section being loaded */
void __cdecl tagpu_datakeys_weapons_clear(void);
void tagpu_datakeys_weapon_read(int id, void* sec, const char* name);
/* GAME thread. The can-engage verdict of 0x49ABB0 filtered by the slot's
   weapon; the order action's unit branch at 0x43F1D4 (0 = stock, 1 = refuse,
   2 = on, 3 = no action); the guidance at 0x49B9EB (1 = steer); the fire gate
   after the per-tick target read (0 = on, 1 = hold, 2 = target dropped). */
int tagpu_datakeys_engage(char* unit, char* target, const char* weapon, int verdict);
int __stdcall tagpu_datakeys_order(char* shooter, char* target);
int __stdcall tagpu_datakeys_steer(const char* weapon);
int tagpu_datakeys_fire_gate(char* unit, int slot_index, const char* weapon, const void* slot);

/* GAME thread: the under-attack notification 0x47F850(unit, kind, arg), which
   answers without calling it for kind 2 inside a harmless weather hit
   (nomapweaponalert). The engine's one site 0x4071D8 calls this once the
   silence is installed; the extra-weapons module's Retaliate calls it always,
   and it is 0x47F850 itself whenever the silence is not installed. */
int __stdcall tagpu_datakeys_alert(char* unit, int kind, int arg);

/* Veterancy (C3): VeterancyThresholds= and VeterancyAccuracyBuffRate=, read
   into the unit-key records. Each site's answer, on the GAME thread in play:
   -1 means "run stock's own instructions" (the type carries no key), any
   other value is the keyed type's, already bounded for the site's arithmetic.
   The sites are in the fail-closed table (tagpu_patches.c, fix_veterancy):
   taken 0x489BFA (L <= 25), dealt 0x499DB5, reload 0x49E468 (L <= 16), lead
   0x48A324 (1 on, 0 off), accuracy 0x49D6EA (the divisor), the capture's
   cost 0x4043D8 (10 + L) and a unit reclaim's step 0x43869D (L + 1), both
   with L <= 13107. */
int __stdcall tagpu_datakeys_vet_taken(const char* unit);
int __stdcall tagpu_datakeys_vet_dealt(const char* unit);
int __stdcall tagpu_datakeys_vet_reload(const char* unit);
int __stdcall tagpu_datakeys_vet_lead(const char* unit);
int __stdcall tagpu_datakeys_vet_accuracy(const char* unit);
int __stdcall tagpu_datakeys_vet_capture_cost(const char* unit);
/* the reclaim step also takes the product's other factors, to bound its own */
int __stdcall tagpu_datakeys_vet_reclaim_step(const char* unit, unsigned workertime,
                                              unsigned maxhp, unsigned ticks);
/* the reload level, keyed or stock's min(kills / 5, 5): the extra-weapons
   module's C reload takes the same one */
int tagpu_datakeys_vet_reload_level(const char* unit);
/* the damage-taken level, keyed or stock's */
unsigned tagpu_datakeys_vet_taken_level(const char* unit);

/* Transported explosions (C4): TransportedExplodeAs= and
   TransportedSelfDestructAs=, read into the unit-key records and resolved to
   weapon IDs at load. GAME thread: whether the unit's type carries either key,
   and the weapon its carried death explodes with (NULL = stock's, the key
   absent for that case). MAIN thread, the menu-time loader's last fold
   (0x42B019): CRC_weapons with the keys' weapon sections folded in. The sites
   are in the fail-closed table (tagpu_patches.c, fix_transported). */
int tagpu_datakeys_tx_keyed(const char* unit);
const char* tagpu_datakeys_tx_weapon(const char* unit, int selfd);
unsigned __stdcall tagpu_datakeys_tx_fold(unsigned crc, void* tdf);

/* DLL attach, before the fail-closed table is written: the unit-key reader
   (0x42D2E0, 0x42BF40, 0x42BF97; and the COB checksum 0x4B6BA0 for the ghost
   mask). 1 when armed. Asked again, it answers the first call's result. */
int tagpu_datakeys_units_install(void);

/* DLL attach, before the engine runs: the nomapweaponalert silence and the
   veterancy panels, each skip-and-log, and the unit-key reader if the table
   has not installed it. */
void tagpu_datakeys_init(void);

/* GAME THREAD, inside an in-play packet fill only. The ghost mask of unit type
   `type`: 1 with `*out` filled when the ghost hides at least one piece, 0 when
   it shows every piece (no row). Bounded by the engine's own UNITINFOCount. */
int tagpu_datakeys_ghost_mask(unsigned type, TAGPU_PK_GHOSTMASK* out);

#endif
