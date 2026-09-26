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
   at attach for the process, each site byte-matched; a mismatch leaves that
   site untouched, skips the sites that rest on it, and logs.

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

/* DLL attach, before the engine runs: installs the FBI reader and the
   nomapweaponalert silence, each skip-and-log. */
void tagpu_datakeys_init(void);

/* GAME THREAD, inside an in-play packet fill only. The ghost mask of unit type
   `type`: 1 with `*out` filled when the ghost hides at least one piece, 0 when
   it shows every piece (no row). Bounded by the engine's own UNITINFOCount. */
int tagpu_datakeys_ghost_mask(unsigned type, TAGPU_PK_GHOSTMASK* out);

#endif
