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
#include <stddef.h>
#include <stdint.h>
#include "tagpu_wikiasset.h"
#include "tagpu_wikistage.h"

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

/* THE CAMERA (techtree.js): canvas pixels = (tree units - (x, y)) * z, (x, y) the tree point at the
   canvas's top-left corner. */
typedef struct { double z, x, y; } WTCAM;

/* techtree.js fit: the whole tree as large as a canvas cw x ch device pixels allows, centred, with
   WT_MARGIN units round it at `s` device pixels a unit */
void tagpu_wikitech_fit(const WIKITECHTREE* T, double cw, double ch, double s, WTCAM* out);

/* techtree.js hit: what is under the tree point (px, py): 1 with `*unit` its order index, `*card`
   the card's order index and `*icon` 1 on a card's icon; 0 over no card. */
int tagpu_wikitech_hit(const WIKITECHTREE* T, double px, double py, int* unit, int* card, int* icon);

/* A title's or description's code-page bytes as UTF-8 into `d` (`n` bytes with the terminator):
   each byte the code point of its value (the mockup's text, which gfx.py reads as Latin-1), or with
   `cp1252` a title's rule, Windows-1252 with '?' for a byte that has no character there
   (gfx.py display_name). Cut at a whole character. */
void tagpu_wikitech_utf8(char* d, size_t n, const unsigned char* s, int cp1252);

/* techtree.js KINDS[kind], "" outside them */
const char* tagpu_wikitech_kind_name(int kind);

/* techtree.js home: the card a unit is drawn on -- its own, else the nearest builder the walk reached
   it from that has one -- as an order index, -1 none */
int tagpu_wikitech_home(const WIKITECHTREE* T, int k);

/* techtree.js pathTo: the walk from the commander to `k`, its order indices into `out`, the
   commander first and `k` last. Returns their count, 0 for no unit, -1 when more than `max`. */
int tagpu_wikitech_path(const WIKITECHTREE* T, int k, int32_t* out, int max);

/* the text roles of a card: its title, its description, its kind line and footer */
enum { WT_ROLE_TITLE, WT_ROLE_BODY, WT_ROLE_LABEL, WT_ROLES };

/* What the drawing reads besides the tree: each role's style in layout.json `styles` and its
   measure, `measure(ctx, role, size, s)` the advance of the UTF-8 string `s` at `size` device
   pixels in that role's face, as tagpu_hudtext_width gives it. */
typedef struct {
    int16_t style[WT_ROLES];
    double (*measure)(void* ctx, int role, double size, const char* s);
    void*   measure_ctx;
} WTART;

/* techtree.js draw, call for op, into `o` (emptied first): the tree under camera `cam` on the stage's
   canvas -- `stage` in HUD units, snapped to device pixels by tagpu_wikistage_canvas, `u` device
   pixels a unit -- with `sel` the selected unit's order index (-1 none) and `all` every builder's
   edge. Every op clips to the canvas; a picture is a HOP_PIC of the unit's WIKITECH picture. */
void tagpu_wikitech_draw(const WIKITECHTREE* T, const WTCAM* cam, int sel, int all, const WTART* art, HUDRECT stage,
                         double u, WIKISTAGEOPS* o);

/* wiki.js drawTech: the stage the entry shows, into `o` (emptied first) -- its fill, then the tree
   as tagpu_wikitech_draw draws it, or with no tree (`T` NULL) the message `msg` centred at nine
   units, in the description's style. */
void tagpu_wikitech_stage(const WIKITECHTREE* T, const char* msg, const WTCAM* cam, int sel, int all, const WTART* art,
                          HUDRECT stage, double u, WIKISTAGEOPS* o);

#endif
