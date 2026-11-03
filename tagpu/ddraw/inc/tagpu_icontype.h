#ifndef TAGPU_ICONTYPE_H
#define TAGPU_ICONTYPE_H
/* tagpu_icontype.h -- what the strategic icons know of a unit's TYPE.

   Past a zoom level BAR's rule decides from a unit's type and the view, a unit
   is drawn as a flat icon of constant screen size instead of, or over, its
   model (research/notes/mega-map-plan.md, landing 3). The rule itself is
   inc/tagpu_icon_rule.h; this header is the per-type half of what it reads.

   THE CLASS TABLE is built on the GAME thread inside the frame packet's
   in-play fill (tagpu_packet_pub.c), once a level, from the UnitDefs and the
   sides' commander names. Every in-play packet carries it (TAGPU_PK_UTYPE,
   indexed by a unit's type row), the render side draws from that copy
   (tagpu_icon.c) and the pick asks the game thread's own table
   (tagpu_zoom.c, the hit test's guard). Nothing here names engine memory. */
#include <stdint.h>

/* BAR's icon types for the nearest TA unit (BAR icontypes.lua): what a type's
   shape and size are chosen by. The order is the packet's encoding. */
enum { TAGPU_ICLS_UNKNOWN, TAGPU_ICLS_COMMANDER, TAGPU_ICLS_FACTORY, TAGPU_ICLS_BUILDING,
       TAGPU_ICLS_AIRCONS, TAGPU_ICLS_AIRCOMBAT, TAGPU_ICLS_AIR, TAGPU_ICLS_CONS,
       TAGPU_ICLS_COMBAT, TAGPU_ICLS_N };

/* TAGPU_PK_UTYPE.icon: the type has no icon of a mod's own set */
#define TAGPU_ICON_NOSET 0xFFu

struct TAGPU_PK_UTYPE;
/* GAME THREAD: the table built for the level in play, `*n` rows -- the level's
   UNITINFOCount, so a model id below that count indexes it. NULL, and *n 0,
   when none is built for this level: before its first in-play publish, after
   its end, or for a level with more types than the table holds. */
const struct TAGPU_PK_UTYPE* tagpu_icontype_table(unsigned* n);

#endif
