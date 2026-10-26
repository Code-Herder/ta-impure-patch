#ifndef TAGPU_WIKIWORLD_H
#define TAGPU_WIKIWORLD_H
/* tagpu_wikiworld -- the producer of the in-game wiki's 3D stage: what the wiki's instances of the
   world passes (tagpu_scene.h) draw their hand-overs from, and the inert answers that stand in
   for the modules only the game's world has.

   The definitions are in tagpu_wikiworld.c under the names tagpu_scene.h gives them, checked
   there against the very prototypes the instances call. Today every source answers "nothing this
   frame", which every pass already handles as a frame it has nothing to draw. Render thread only,
   as the hand-overs it stands for. */

#include "tagpu_wikistage.h"            /* WIKISTAGE_UNITS, WIKISTAGE_SITES */

/* THE 3D FEATURES ONE STAGE CAN HOLD: a feature whose section names a model (a wreck such as
   `armmex_dead`) is drawn through the unit pass, so each takes a bake entry beside the units. The
   stage description's reader refuses a description with more. */
#define WIKIWORLD_FEAT3D   64u

/* THE MOST POSED OBJECTS ONE FRAME OF THE STAGE DRAWS: every unit, the ghost of every gesture
   site, the placement ghost and every 3D feature. It is the wiki instances' TAGPU_PB_FRAMEMAX
   (tagpu_scene.h), which sizes the bake's caches and the unit pass's vertex-buffer table, so a
   frame of the stage can never evict an entry it has asked for (tagpu_posebake.h). */
#define WIKIWORLD_FRAMEMAX ((unsigned)WIKISTAGE_UNITS + (unsigned)WIKISTAGE_SITES + 1u + \
                            WIKIWORLD_FEAT3D)

#endif
