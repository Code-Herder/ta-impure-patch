#ifndef TASIM_WORLDQ_H
#define TASIM_WORLDQ_H
/* tasim_worldq.c -- worldq.h over the shared simulation core's WIKISIM: the world the in-game wiki plays,
   answered as the engine answers it, so the rules above WORLDQ (tasim_snap.c, tasim_place.c, tasim_sweep.c)
   run on the wiki's world exactly as on the game's. Engine-free, as the whole core is (tasim.h).

   THE ENGINE'S NUMBERING, ON THE CORE'S WORLD. A slot is a unit's place in WIKISIM.unit plus 1, so slot 0 is
   none as in the engine; a unit's index (UnitInGameIndex) is its slot. A type is the entry's type plus 1, so
   type 0 is none as in the engine. Positions are 16.16 world units, the core's doubles truncated
   (tasim_fix16), at altitude 0: the stage is flat. The metal of a cell and the threshold are the scene's
   values held to 0..255 as the engine's bytes are.

   THE ACTIONS are the core's own bytes (CORE_ACT_*): a list holds move, build, reclaim and guard orders, and
   the VTOL and resurrect actions exist so a rule that asks for them gets an answer no list ever holds. The core
   has no factory queue, so it has no BUILDINGBUILD byte. The map has no wind and no tide. */
#include "worldq.h"
#include "tagpu_wikisim.h"

enum { CORE_ACT_MOVE = 1, CORE_ACT_BUILD, CORE_ACT_RECLAIM, CORE_ACT_GUARD, CORE_ACT_VTOL_BUILD,
       CORE_ACT_VTOL_RECLAIM, CORE_ACT_RESURRECT };

/* `lit` and `con`, NULL from the open, are the seat's sight that observe, block_lit and radar_step answer from,
   the caller's for the call: its lit grid (tagpu_wikisim_sight_lit, (W >> 1) x (H >> 1) bytes) and its contacts
   (tagpu_wikisim_contacts, one byte a unit in slot order). */
typedef struct WQ_CORE { WIKISIM* s; int seat; const uint8_t* lit; const uint8_t* con; } WQ_CORE;

/* Opens seat `seat`'s view of `s` into `out` over `ctx`, both the caller's for one call; 0 for a seat out
   of 0..WIKI_SEATS-1. */
int tasim_worldq_open(WQ_CORE* ctx, WORLDQ* out, WIKISIM* s, int seat);

#endif
