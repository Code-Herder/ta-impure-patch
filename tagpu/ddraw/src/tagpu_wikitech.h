#ifndef TAGPU_WIKITECH_H
#define TAGPU_WIKITECH_H
/* tagpu_wikitech -- the tech tree entry's tree: one side of the loaded tech tree (WIKITECH,
   tagpu_wikiasset.h) made into a research tree and laid out, as the mockup's
   tools/wiki/mockup/techtree.js `model` and `layout` make it (research/notes/ingame-wiki.md,
   *The tech tree entry*). techtree.js is the specification, rule for rule and number for number:
   tools/wiki/assetcheck.py --side pairs this file's model and layout with it over every setup's
   tree (wikiassettest techlay).

   PURE. It names no engine address, holds no global and reads only its arguments: the published
   tree, immutable and never freed. Pure C99 and the C library, so the native test compiles it.

   Every index is an ORDER index (the side's walk, 0 the commander) unless named a unit index; each
   list is `n` entries of a pool, every entry an order index below `n`. */
#include <stdint.h>
#include "tagpu_wikiasset.h"

/* a unit's kind, for the panel's groups (techtree.js KINDS, in its order) */
enum { WT_COMMANDER, WT_FACTORY, WT_BUILDER, WT_ECONOMY, WT_DEFENSE, WT_SUPPORT, WT_LAND, WT_AIR, WT_SEA,
       WT_UPGRADE, WT_KINDS };

typedef struct { uint32_t at, n; } WTLIST;     /* entries pool[at .. at + n) */

typedef struct {
    const WIKITECH* t;
    int             side;
    int             n;                  /* the walk's length                                    */
    const uint16_t* unit;               /* n unit indices, the side's walk                       */
    uint8_t*        kind;               /* WT_*                                                  */
    uint8_t*        upgrade;            /* never stands on the field: shown on its builder       */
    uint8_t*        upgradable;
    uint8_t*        tier;               /* 1..9: the category's LEVELn, else builders crossed + 1,
                                           at most 4                                             */
    int32_t*        first;              /* the builder the walk reached it from, -1 the commander */
    WTLIST*         builds;             /* its menu less its upgrades                            */
    WTLIST*         upgrades;           /* its menu's upgrades                                   */
    WTLIST*         parents;            /* every builder whose menu holds it, in walk order      */
    WTLIST*         unlocks;            /* what of `builds` it is the first to reach             */
    WTLIST*         icons;              /* a card's icons: its unlocks that are no card, then its
                                           upgrades (techtree.js cardIcons); empty on no card    */
    int32_t*        pool;
    int             nspine;             /* the cards: the commander, every builder and factory   */
    int32_t*        spine;              /* nspine order indices, in walk order                   */
    int32_t*        sp;                 /* per order index: its card's parent card, -1 none      */
    int32_t*        card;               /* per order index: its index in `spine`, -1 no card     */
    /* THE LAYOUT, in wiki units: per card, [x, y, w, h] from the first card's corner */
    double         (*pos)[4];
    double          w, h;               /* the tree's bounds                                     */
} WIKITECHTREE;

/* the card's measures (techtree.js C) */
#define WT_W      118.0     /* a card's width                                     */
#define WT_PITCH  150.0     /* from one column's left to the next's               */
#define WT_GAP    6.0       /* the least space between two cards of a column      */
#define WT_PAD    3.0
#define WT_HEAD   30.0      /* the header: the picture, title, subtitle, kind line */
#define WT_PIC    24.0
#define WT_ICON   13.0
#define WT_IGAP   1.0
#define WT_PER    8         /* icons a row                                        */
#define WT_FOOT   9.0
#define WT_EDGE_Y 15.0      /* where an edge meets a card                         */
#define WT_MARGIN 8.0       /* units round the fitted tree                        */

/* Side `side` of `t` made into a tree and laid out: 1, or 0 with nothing allocated when the side
   does not exist or memory ran out. tagpu_wikitech_free releases it. */
int  tagpu_wikitech_tree(const WIKITECH* t, int side, WIKITECHTREE* out);
void tagpu_wikitech_free(WIKITECHTREE* tr);

/* A card's height for `n` icons (techtree.js cardH): exact before anything is drawn. */
double tagpu_wikitech_card_h(uint32_t n);

#endif
