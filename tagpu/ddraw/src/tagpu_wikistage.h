#ifndef TAGPU_WIKISTAGE_H
#define TAGPU_WIKISTAGE_H
/* tagpu_wikistage -- the in-game wiki's stage: one view of the wiki's simulation drawn as the
   replay HUD's ops, as the mockup's tools/wiki/mockup/stage.js draws it into a canvas. stage.js
   is the specification, call for op and in its paint order: tools/wiki/stagecheck.mjs plays every
   demo through sim.js and stage.js and through tagpu_wikisim.c and this file, and pairs each
   canvas call with its op within 1/64 of a pixel.

   PURE. It names no engine address, holds no global and reads only its arguments: the view by
   value (WIKISTAGEVIEW) and the sidebar layout and art the caller resolves for it, both immutable,
   the stage's rect in HUD units and u, the device pixels a unit. So the render thread can build
   the picture from a packet record while the game thread steps the simulation that made it, and
   a native build of this file is the check's.

   Every op is in HUD units from the screen's top left, as tagpu_huddraw.c draws them, and clips
   to the stage's canvas as the canvas clips what is drawn on it. The canvas is the stage's rect
   snapped to device pixels as Chrome snaps an element (tagpu_wikistage_canvas); every position
   inside it is computed in device pixels from its corner, in double, by stage.js's arithmetic.

   Pure C99 and the C library, so tools/wiki/stagecheck.mjs compiles it natively. */
#include <stdint.h>
#include "tagpu_hudlay.h"
#include "tagpu_wikisim.h"

/* ---- the view by value: WIKIVIEW with every array bounded ---------------------------------- */

#define WIKISTAGE_MAP     128                   /* cells a side                               */
#define WIKISTAGE_VENTS   256
#define WIKISTAGE_UNITS   128
#define WIKISTAGE_SITES   PLACE_MAX_SITES       /* a gesture never has more                   */
#define WIKISTAGE_QUEUED  256
#define WIKISTAGE_LINES   512
#define WIKISTAGE_CAPTION 256                   /* 255 bytes and the terminator               */
#define WIKISTAGE_LABEL   WIKI_ID_MAX           /* 23 bytes and the terminator                */

/* what the copy could not carry, one bit for each bound */
#define WIKISTAGE_TRUNC_MAP     0x001u          /* the map is wider or taller: its top left    */
#define WIKISTAGE_TRUNC_METAL   0x002u          /* a metal value is not a whole number 0..255  */
#define WIKISTAGE_TRUNC_VENTS   0x004u
#define WIKISTAGE_TRUNC_UNITS   0x008u
#define WIKISTAGE_TRUNC_SITES   0x010u
#define WIKISTAGE_TRUNC_QUEUED  0x020u
#define WIKISTAGE_TRUNC_LINES   0x040u
#define WIKISTAGE_TRUNC_CAPTION 0x080u
#define WIKISTAGE_TRUNC_LABEL   0x100u          /* a label or a type's name, cut at a character */

typedef struct {
    float   x, z;                       /* its centre, world units                         */
    float   hx, hz;                     /* its facing, a unit vector (tagpu_wikisim.h)     */
    float   built, reclaimed;           /* fractions                                       */
    uint8_t type;                       /* its type: an index into `type` below            */
    uint8_t fx, fz;                     /* footprint, cells                                */
    uint8_t owner;                      /* 255: none of the eight players                  */
    uint8_t selected;
    int16_t tx, tz;                     /* its centre truncated toward zero from the
                                           simulation's double, as the minimap's dot takes it
                                           (tagpu_wikiside.h): the float above may round
                                           across a whole unit                              */
    char    label[WIKISTAGE_LABEL];     /* its label, else its type's name                 */
} WIKISTAGEUNIT;

/* the ghost, a gesture's site or a queued site: a building's centre and footprint */
typedef struct {
    float    x, z;
    uint8_t  type, fx, fz;
    uint8_t  ok;                        /* the ghost: green, else red                      */
    uint8_t  state;                     /* a gesture's site: WIKI_SITE_*                   */
    uint8_t  owner;                     /* a queued site: its order's owner; 255 none      */
    uint16_t order;                     /* a gesture's site: its place among the orders the
                                           release adds, 0 for none                        */
} WIKISTAGESITE;

typedef struct { float from_x, from_z, to_x, to_z; uint8_t kind; } WIKISTAGELINE;

/* WHICH LAYOUT THE SIMULATION RUNS ON, as the view names it: a value, so the record stays a
   record of values (tagpu_wikipk.h). The render thread resolves it to a layout it holds itself
   (WIKISTAGEART.side_layout): the wiki data's fixture, or the published WIKISIDE's own. */
enum { WIKISTAGE_LAYOUT_NONE, WIKISTAGE_LAYOUT_FIXTURE, WIKISTAGE_LAYOUT_MOD, WIKISTAGE_LAYOUT_N };

/* THE SIDEBAR, WIKIVSIDE as the stage draws it (tagpu_wikisim.h): the column's rows, the side
   shown, the routed screen, each gadget's state and the unit under the cursor, the last as an
   index of the view's `unit`. The copy holds each to its array; the render thread bounds `index`
   against the layout's nside, `screen` against its nscreen and `nstate` against the screen's
   gadgets where it reads them. */
typedef struct {
    int16_t h;                          /* the layout's height: 480 .. WIKI_SIDE_ROWS      */
    int8_t  index;                      /* the layout side shown (WIKIVSIDE.index): its
                                           prefix names the whitebox buttons and its
                                           PANELSIDE is drawn; -1 none. Data: the render
                                           thread bounds it against the layout's sides    */
    int16_t screen;                     /* an index into the view's layout's `screen`      */
    int16_t nstate;
    int16_t under;                      /* an index of `unit`, -1 for none                 */
    int8_t  state[WIKI_SIDE_GADGETS];   /* -1 not drawn, 0 normal, 1 pressed or the current
                                           tab, 2 greyed                                   */
} WIKISTAGESIDE;

typedef struct {
    uint32_t       trunc;               /* WIKISTAGE_TRUNC_*                               */
    int32_t        tick;                /* the simulation's tick this view is of: the copy
                                           leaves it, its caller sets it                    */
    int16_t        w, h;                /* cells; 0 when there is no view                  */
    double         surface_metal;
    uint8_t        metal[WIKISTAGE_MAP * WIKISTAGE_MAP];    /* w * h, row-major            */
    uint8_t        fog[WIKISTAGE_MAP * WIKISTAGE_MAP];      /* 0 clear, 1 grey, 2 black    */
    int16_t        nvent, nunit, nsite, nqueued, nline;
    float          vent[WIKISTAGE_VENTS][2];                /* each vent cell's centre     */
    WIKISTAGEUNIT  unit[WIKISTAGE_UNITS];
    uint8_t        has_ghost;
    WIKISTAGESITE  ghost;
    WIKISTAGESITE  site[WIKISTAGE_SITES];
    int8_t         shape;               /* PLACE_*, PLACE_NONE for none                    */
    int16_t        spacing;             /* the armed building's, -1 when none is armed     */
    WIKISTAGESITE  queued[WIKISTAGE_QUEUED];
    WIKISTAGELINE  line[WIKISTAGE_LINES];
    double         cursor_x, cursor_z;  /* double: the cursor's icon box is snapped to a
                                           pixel, which a float's error could move        */
    uint8_t        cursor_shape;        /* WIKI_CURSOR_*                                   */
    int32_t        cursor_since;        /* the tick the shape began, its animation's start */
    uint8_t        held[WIKI_NKEYS];
    uint8_t        has_drag;
    float          drag_x0, drag_z0, drag_x1, drag_z1;
    uint8_t        has_caption;
    char           caption[WIKISTAGE_CAPTION];
    double         pace;
    int16_t        mode;                /* the armed type, or -1: contextual               */
    uint8_t        has_side;            /* the sidebar is on: the stage draws its column   */
    uint8_t        layout_src;          /* WIKISTAGE_LAYOUT_*: the layout `side` indexes   */
    WIKISTAGESIDE  side;
    int16_t        locked;              /* the type the demo stopped on (WIKIVIEW.locked),
                                           with the sidebar on or off, an index into
                                           `type`; -1 none: the page shows its badge       */
    int16_t        ntype;
    char           type[WIKI_MAX_DEFS][WIKISTAGE_LABEL];    /* the types' names: a queued
                                           site's and the ghost's label                    */
} WIKISTAGEVIEW;

/* THE COPY. `v` (NULL for none) and the entry `e` it was made from into `out`, every array to its
   bound and every string cut at a character, by value: a WIKIVIEW's arrays live in its WIKISIM
   and hold only until the next call on it, so nothing that outlives the call may point into
   them. `layout_src` (WIKISTAGE_LAYOUT_*) names the layout the simulation was started with
   (tagpu_wikisim_init), kept as the view's while its sidebar is on. Returns the
   WIKISTAGE_TRUNC_* bits, which are also out->trunc. */
uint32_t tagpu_wikistage_copy(WIKISTAGEVIEW* out, const WIKIVIEW* v, const WIKIENTRY* e, int layout_src);

/* ---- the picture ---------------------------------------------------------------------------- */

/* The wiki's own op budget, apart from the replay HUD's HUD_MAXOP: a 40 x 30 map's cells, grid
   and fog come to about 3750, the whitebox minimap's cells, fog and dots to at most
   2 x 1200 + 128 more and the sidebar's buttons to about 200; a 128 x 128 map's cells and fog
   alone would take every op. */
#define WIKI_MAXOP   16384
#define WIKI_MAXRUN  1024
#define WIKI_TEXTMAX 16384
/* the groups: one a greyed button of the whitebox sidebar, so a screen's gadgets bound them,
   and the HUD pass draws at most HUD_MAXGRP */
#define WIKI_MAXGRP  WIKI_SIDE_GADGETS

/* THE THREE OP RANGES (research/notes/ingame-wiki-3d-stage-plan.md, section 3): every op belongs to
   one, stage.js's `o.layer` names, and the frame draws either R0 then R1 -- the whitebox, every op
   in order -- or R0, the stage's 3D image, then R2, the ops tagpu_wikistage_keep keeps. */
#define WIKISTAGE_IN_R0     0             /* under the image: the stage's background, a message      */
#define WIKISTAGE_IN_GROUND 1             /* the whitebox ground: cells, grid, vents, fog            */
#define WIKISTAGE_IN_BOX    2             /* a unit's, site's or ghost's box, by its type (`rtype`)  */
#define WIKISTAGE_IN_LABEL  3             /* a queued site's name: the whitebox's alone              */
#define WIKISTAGE_IN_OVER   4             /* the overlays: lines, squares, selection, the cursor...  */
#define WIKISTAGE_IN_SIDE   5             /* the left sidebar, left of the map: under the image      */

typedef struct {
    int     n, nrun, ntext;
    int     dropped;                    /* the ops this build had no room for              */
    int     logged;                     /* kept across builds: a dropping build was logged */
    int     cursor;                     /* the op that draws the cursor, -1 when none does */
    int     ngrp;                       /* the groups the ops' `grp` name, 1-based          */
    float   grp[WIKI_MAXGRP];           /* each group's opacity (HUDOPLIST `grp`)           */
    HUDOP   op[WIKI_MAXOP];
    uint8_t range[WIKI_MAXOP];          /* each op's WIKISTAGE_IN_*          */
    uint8_t rtype[WIKI_MAXOP];          /* a WIKISTAGE_IN_BOX op's unit type, an index into the
                                           view's `type`                                   */
    HUDRUN  run[WIKI_MAXRUN];
    char    text[WIKI_TEXTMAX];
} WIKISTAGEOPS;                         /* about 0.9 MB: never on a stack                  */

/* A string's advance in the stage's text style at `size` device pixels, in device pixels: the
   faces' measure (tagpu_hudtext_width), which this file has no faces for. */
typedef double (*WIKISTAGEMEASURE)(void* ctx, double size, const char* s);
typedef void   (*WIKISTAGELOG)(void* ctx, const char* line);

/* THE GAME'S CURSORS, as the stage draws them: each frame of the sequences the shapes show
   (tagpu_wikisim.h, WIKI_CURSOR_*), decoded once into one cell of the sprite sheet the HUD pass
   holds (HUD_TEX_SPRITE, tagpu_huddraw.h), cell k at x = k * HUD_SPRITE_CELL. A frame is the
   GAF frame's size and hotspot and the 30 Hz ticks it stays up (the sequence's frame table's
   second dword, which the animator 0x4B8B30 / 0x4B8BF0 counts down). */
typedef struct {
    int16_t  w, h;                      /* art pixels, each at most HUD_SPRITE_CELL        */
    int16_t  hx, hy;                    /* the hotspot: the pointer's place in the frame,
                                           signed, as sharp_cursor subtracts it            */
    uint16_t dur;                       /* 30 Hz ticks                                     */
} WIKISTAGESPR;

typedef struct {
    uint8_t  n;                         /* frames; 0: the shape draws its kit icon         */
    uint8_t  first;                     /* its frame 0's cell                              */
    uint8_t  loop;                      /* the sequence's header byte +2: back to frame 0
                                           past the last, else nothing is drawn there      */
} WIKISTAGESEQ;

/* What the picture needs of the loaded art, by the caller. */
typedef struct {
    int16_t          style;             /* layout.json's `toast` style: the stage's text   */
    WIKISTAGEMEASURE measure;           /* never NULL                                      */
    void*            measure_ctx;
    int16_t          cursor[3];         /* the kit's cursor_arrow, cursor_move and
                                           cursor_build, by WIKI_CURSOR_*; -1 absent: the
                                           icons a shape whose game cursor did not load
                                           draws instead                                   */
    float            cursor_w[3], cursor_h[3];  /* their sizes in units: pw / scale        */
    WIKISTAGESEQ     seq[3];            /* the game's cursor of each shape                 */
    WIKISTAGESPR     spr[HUD_SPRITE_CELLS];
    float            spr_scale;         /* device pixels an art pixel: the game cursor's,
                                           tagpu_gui.on's cursorscale= (1 by default)      */
    WIKISTAGELOG     log;               /* NULL: none                                      */
    void*            log_ctx;
    /* THE LAYOUT THE VIEW'S `layout_src` NAMES, resolved by the caller to one it holds (NULL:
       none, and a sidebar draws its column and minimap alone). It must be immutable while the
       build runs; the DLL's are WIKIDATA.fixture and the published WIKISIDE's, never freed. */
    const WIKILAYOUT*      side_layout;
    /* THE SIDEBAR IN THE MOD'S OWN PICTURES, or NULL for the whitebox column: the published
       WIKISIDE whose column this frame composed (tagpu_wikiside_compose), used only for a view
       on the mod's layout that is this block's own (`side_layout` == &side->layout), since its
       pictures are parallel to that layout's gadgets. The column is then one HOP_SIDE op, and a
       gadget whose picture is a label a whitebox button over it (wiki.js sideArt).
       `side_minimap` 0: the composed column has no minimap (no base picture for this stage),
       and the whitebox's is drawn over it. */
    const struct WIKISIDE* side;
    int              side_minimap;
} WIKISTAGEART;

/* THE ENGINE'S CURSOR ANIMATION: the frame `elapsed` 30 Hz ticks after SetUICursor 0x4AB400
   installed a sequence of `n` frames lasting `dur` ticks each. 0x4B8B30 starts it at frame 0
   with frame 0's ticks to count down; 0x4B8BF0, from the UI's update 0x4AB5D0, takes the ticks
   elapsed off and, at or below zero, steps to the next frame and adds that frame's ticks -- back
   to frame 0 past the last when the sequence loops, else it drops the sequence and the cursor
   draws nothing (-1). A sequence of one frame never steps (0x4B8BFB). DISASSEMBLED 2026-10-27. */
int tagpu_wikistage_cursor_frame(const WIKISTAGESPR* frames, int n, int loop, long elapsed);

/* THE CANVAS: the stage's rect `stage`, in units, at `u` device pixels a unit, snapped to whole
   device pixels as Chrome snaps the canvas element (tagpu_hud_snap, tagpu_hud_edge): its top left
   (`*x0`, `*y0`) and its size, in device pixels. */
void tagpu_wikistage_canvas(HUDRECT stage, double u, double* x0, double* y0, double* w, double* h);

/* THE FIT, stage.js `fit` over `content`: a map of `w` x `h` cells, and with the sidebar
   (`side_h` its rows, 0 for none) the 128-unit column left of it, in a canvas `stage_w` x
   `stage_h` device pixels at `u` pixels a unit, letterboxed with a pad of 6 units. The content
   rect is x in [x0, x0 + cw), z in [0, ch), world units: the map's, or with the column x0 = -128,
   cw = 128 + the map's width and ch the taller of the map and the column; the map stays at the
   top. `s` is device pixels a world unit and (`ox`, `oz`) the world origin -- the map's corner --
   from the canvas's top left, for the map and the column alike. ONE FUNCTION ON BOTH THREADS:
   the render thread draws the stage with it and the game thread turns a click on the stage into
   a world point with it, from the same frame's canvas, so the two cannot disagree. */
typedef struct { double s, ox, oz, x0, cw, ch; } WIKISTAGEFIT;
void tagpu_wikistage_fit(int w, int h, int side_h, double stage_w, double stage_h, double u, WIKISTAGEFIT* out);

/* THE IMAGE'S RECT (stage.js `draw`, the image's rect): where the stage's 3D target is composited, the
   map's rect of the fit (`side_h` as tagpu_wikistage_fit takes it) on the canvas of `stage`, each
   edge snapped to the device pixel as stage.js snaps it -- [*x0, *x1) x [*y0, *y1) in device
   pixels from the screen's top left. The composite and every op of the frame come from this one
   fit, so the image and the overlays cannot disagree by more than the snap's half pixel. */
void tagpu_wikistage_image(int w, int h, int side_h, HUDRECT stage, double u, int* x0, int* y0, int* x1, int* y1);

/* WHERE AN OP IS DRAWN THIS FRAME: 1 under the image, 2 over it, 0 not at all. `image` 0 is the
   whitebox (R0 and R1: every op, in order, under -- there is no image to be over); with `image` 1,
   R0 and the sidebar, which lies left of the map's rect, are under, and so is the whitebox ground
   when the image carries none (`ground` 0), and a box is
   over when its type did not resolve (bit t of `resolved`, WIKI_MAX_DEFS bits), every other overlay
   is over and a queued site's label is never drawn. stage.js `draw`'s `shows`, in C. */
int tagpu_wikistage_keep(const WIKISTAGEOPS* o, int i, int image, int ground, const uint32_t* resolved);

/* The stage's ops for `v` into `out`, in stage.js's paint order, on the canvas of `stage` at `u`:
   with the sidebar, its column (WIKISTAGE_IN_SIDE) after the background and before the map.
   A view with no map draws the stage's ground and `msg` (NULL for none). WIKI_MAXOP BOUNDS IT: an
   op past the budget is dropped and counted in out->dropped; the build that first drops is
   logged through art->log, once while out->logged stays set; and a frame that dropped any is
   edged in red, in a slot the budget keeps for it, so the loss shows. `cursor` 0 leaves the cursor
   out (stage.js's o.cursor false); out->cursor names the op that draws it. Returns out->n. */
int tagpu_wikistage_build(const WIKISTAGEVIEW* v, const char* msg, HUDRECT stage, double u, int cursor,
                          const WIKISTAGEART* art, WIKISTAGEOPS* out);

#endif
