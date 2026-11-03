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

/* ---- THE ICON ART ----------------------------------------------------
   One 256x256 atlas of palette indices: the radar logo's frames (`radlogo`,
   one per logo colour, the minimap's dot) and, with a mod's icon set, its
   icons. Every texel no frame draws holds `key`; `fill` and `sel` are the two
   markers a set icon's FillColor and SelectedColor texels are rewritten to.
   The three are the smallest indices no frame's drawn texel holds, so a
   marker can never be a colour of the art.

   WRITTEN ONCE, ON THE GAME THREAD, at the first in-play fill of the process,
   and published by a release store; never written or freed after it. A
   pointer taken after tagpu_icontype_art() answered stays valid and
   unchanging for the process, on any thread. */
#define TAGPU_ICONART_W     256
#define TAGPU_ICONART_H     256
#define TAGPU_ICONART_LOGOS 16
#define TAGPU_ICONART_SETS  64
typedef struct TAGPU_ICONRECT {
    uint16_t x, y, w, h;     /* the frame's texels in the atlas               */
    int16_t  hx, hy;         /* its hotspot                                    */
} TAGPU_ICONRECT;
typedef struct TAGPU_ICONART {
    const unsigned char* pix; int w, h;      /* TAGPU_ICONART_W x _H indices    */
    int key, fill, sel;                      /* markers no art texel holds       */
    int nlogo;                               /* radar logo frames, by logo colour */
    TAGPU_ICONRECT logo[TAGPU_ICONART_LOGOS];
    unsigned char team[TAGPU_ICONART_LOGOS]; /* each logo's team colour: the
                                                frame's texel at its hotspot   */
    int nset;                                /* a mod set's icons; 0 = none     */
    TAGPU_ICONRECT set[TAGPU_ICONART_SETS];
    int none, unknown;                       /* set indices, -1 = absent        */
    int selcol;                              /* what a `sel` texel shows when its
                                                unit is selected: the set's own
                                                SelectedColor                    */
} TAGPU_ICONART;
/* Any thread: the art, NULL until the game thread has published it. */
const TAGPU_ICONART* tagpu_icontype_art(void);

#endif
