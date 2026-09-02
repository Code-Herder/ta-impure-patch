#ifndef TAGPU_CAT_H
#define TAGPU_CAT_H

#include "tagpu.h"

/* tagpu_cat — the live unit and feature catalogues, on demand.

   Validation layer 2 of `tacli scenario`: what a scenario may name. A JSON file
   is only safe to spawn from if its names exist in *this* game, and the only
   authority on that is the game's own definition tables — a mod that swaps what
   index 42 means changes them, and the catalogue changes with it. Design:
   research/notes/scenario-format.md.

   OFF BY DEFAULT and one-shot, exactly like tagpu_ui and tagpu_peek: nothing
   happens until a trigger file appears next to the exe. It is deleted, the walk
   runs once, and the result is written atomically (tmp + rename) so a reader can
   never see a half-built file.

       tagpu_units.trigger    -> tagpu_units.json
       tagpu_features.trigger -> tagpu_features.json

   The chains, both read-only and pointer-guarded:

       main  = *(void**)0x511DE8
       count = *(unsigned*)(main + 0x1438F)   UNITINFOCount
       defs  =  *(char**)(main + 0x1439B)     UnitDefStruct[], stride 0x249
       fcnt  =      *(int*)(main + 0x14253)   NumFeatureDefs
       fdefs =  *(char**)(main + 0x1426F)     FeatureDefStruct[], stride 0x100

   The feature side is the one this fork already renders through
   (tagpu_native.c); the unit array base and stride were taken from tamem.h and
   are checked *in the walk itself*: a live unit's UnitDefStruct* (unit+0x92)
   must equal defs + UnitID*0x249, and the result says whether that held. A
   catalogue that cannot prove its own stride is worth nothing. */

void tagpu_cat_frame(const TAGPU_FRAME* f);

#endif
