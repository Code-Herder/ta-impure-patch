#ifndef TAGPU_WIKISIDE_H
#define TAGPU_WIKISIDE_H
/* tagpu_wikiside -- the in-game wiki stage's left sidebar in the running mod's own pictures: one
   view's column composed as the engine composes its sidebar, in palette indices, then through the
   mod's palette into the RGBA the stage's HOP_SIDE op draws (tagpu_wikistage.c). The mockup's
   tools/wiki/mockup/side.js `compose` and `rgba` are the specification, line for line, and
   tools/wiki/sidecheck.mjs compares the two byte for byte over every demo; the engine facts each
   step rests on are stated there and in tagpu_wikiasset.h.

   PURE AND SAFE ON THE RENDER THREAD. No global, no engine address, no allocation: it reads the
   sidebar's art (WIKISIDE, one block immutable once published and never freed), the view by value
   (WIKISTAGEVIEW, a record's copy) and the stage's minimap picture (WIKI3DSTAGE.minimap, in the
   asset arena, immutable and never freed), and writes only `out`. Every value it reads out of
   them is data until bounded: a frame's rect against the sheet, a screen against the layout, a
   state against its array, a palette index against 0..255, a fog cell against the map, the
   minimap picture against its size, and every write against the column.

   Pure C99 and the C library, so sidecheck.mjs compiles it natively. Its arithmetic is integer:
   side.js truncates double quotients of whole numbers below 2^23 in magnitude by divisors of at
   most 2048 (a map is at most 128 cells of 16 units), and such a quotient, when it is not whole,
   lies at least 1/2048 from every whole number, far beyond a double's rounding there; so C's
   integer division, which truncates toward zero as Math.trunc does, gives the same numbers. */
#include "tagpu_wikiasset.h"
#include "tagpu_wikistage.h"

#define WIKISIDE_MINIMAP 126    /* 0x466780: the minimap picture's long side                 */

/* THE MINIMAP'S PICTURE, 0x466780: the long side WIKISIDE_MINIMAP pixels, the short one
   trunc(126 * short / long), centred on the short axis inside the 128 x 128 square. The extent
   (W, H world units, each positive) is THE STAGE MAP, where the engine uses its scroll extent:
   that extent would cut the stage's bottom rows off the minimap. */
typedef struct { int w, h, offx, offy; } WIKISIDEMAP;
static inline void tagpu_wikiside_geometry(int W, int H, WIKISIDEMAP* m)
{
    if (W >= H) {
        m->w = WIKISIDE_MINIMAP;
        m->h = WIKISIDE_MINIMAP * H / W;
        m->offx = 0;
        m->offy = (WIKISIDE_MINIMAP - m->h) / 2;
    } else {
        m->w = WIKISIDE_MINIMAP * W / H;
        m->h = WIKISIDE_MINIMAP;
        m->offx = (WIKISIDE_MINIMAP - m->w) / 2;
        m->offy = 0;
    }
}

/* A unit's dot in the picture (0x466DC0): its world point truncated to whole units (`tx`, `tz`,
   WIKISTAGEUNIT), flat on the stage, scaled by the picture over the extent and truncated again,
   as the engine's integer arithmetic does. */
static inline void tagpu_wikiside_dot(int tx, int tz, const WIKISIDEMAP* m, int W, int H, int* px, int* py)
{
    *px = tx * m->w / W;
    *py = tz * m->h / H;
}

/* The composed column: `h` rows of WIKI_SIDE_W palette indices and their coverage, 1 where a
   pixel is painted -- which the engine's black clear makes every pixel of the column. About
   256 KB: never on a stack. */
typedef struct {
    int           h;                    /* the view's column, 1 .. WIKI_SIDE_ROWS; 0 none        */
    int           minimap;              /* the minimap's picture is composed in it; 0 leaves the
                                           minimap to the stage's whitebox ops
                                           (WIKISTAGEART.side_minimap)                           */
    unsigned char idx[WIKI_SIDE_W * WIKI_SIDE_ROWS];
    unsigned char a[WIKI_SIDE_W * WIKI_SIDE_ROWS];
} WIKISIDECOL;

/* side.js `compose`: view `v`'s column in `art`'s pictures, into `out`. `base` is the minimap's
   base picture of this stage (WIKI3DSTAGE.minimap, `nbase` = mm_w * mm_h bytes) or NULL, when the
   minimap is left to the whitebox; the extent is the view's map, the stage map. In order: the
   engine's black (palette index 0) over the whole column; PANELSIDE; the minimap -- the base
   under the fog, the dots, the camera box; the routed screen -- its panel picture, then each
   drawn gadget's frame for its state, a greyed one tinted. Returns out->h, or 0 with nothing
   composed when the view has no sidebar, its layout is not the art's own (the pictures are
   parallel to that layout's gadgets), its rows are out of 1..WIKI_SIDE_ROWS or the art has no
   sheet. */
int  tagpu_wikiside_compose(const WIKISIDE* art, const WIKISTAGEVIEW* v, const unsigned char* base, int nbase,
                            WIKISIDECOL* out);

/* side.js `rgba`: the composed column through the art's palette into `out`, WIKI_SIDE_W x c->h
   RGBA8 texels row by row (4 * WIKI_SIDE_W * c->h bytes), alpha 255 where painted and the texel
   0 where not -- so premultiplied as well. */
void tagpu_wikiside_rgba(const WIKISIDE* art, const WIKISIDECOL* c, unsigned char* out);

#endif
