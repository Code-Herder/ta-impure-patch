#ifndef TAGPU_HUDLAY_H
#define TAGPU_HUDLAY_H
/* tagpu_hudlay -- the replay HUD's layout rules: tools/hudart/mockup/hud.js, in C, over the same
   art/hud/layout.json and art/hud/kit.json. research/notes/hud-layout.md states the rules both
   implement; tools/hudart/laycheck.py compares this file's rectangles with the mockup's.

   A PURE FUNCTION OF ITS INPUTS. tagpu_hudlay_build reads the loaded files and the state it is
   given, and nothing else: no engine memory, no global, no clock. So the render thread's frame
   and tools/hudart/'s native build of this file get the same rectangles for the same state, and
   the frame's HOP_HOT ops are the hit map a click is tested against (tagpu_hudstate_drawn). A HUDLAY is immutable once loaded and is
   never freed while the HUD can draw (tagpu_json.h says why that is enough).

   Pure C99 and the C library, like tagpu_json.c, so tools/hudart/ compiles it natively.

   Every coordinate is in HUD units, from the screen's top left. */
#include <math.h>
#include <stdint.h>
#include "tagpu_json.h"

#define HUD_MAXSEAT  16
#define HUD_MAXMARK  320
#define HUD_KEYLEN   24
#define HUD_MAXHOT   128        /* the hit map's controls: TAGPU_CMD_HUDHITS                  */
#define HUD_MAXFORK  3          /* the forks drawn: TAGPU_PK_HUD_FORKS                        */

typedef struct { float x, y, w, h; } HUDRECT;

typedef struct {
    char     name[32];          /* UTF-8 */
    uint32_t colour;            /* 0xRRGGBB */
    float    units;
    int      lost;              /* the commander is lost: the skull, the name padded for it */
    int      out;               /* the player is out of the game: the row dimmed            */
    float    m, mmax, mi, mo;   /* metal stored, storage, income, spending                  */
    float    e, emax, ei, eo;   /* the same, energy                                         */
} HUDSEAT;

enum { HUD_MK_DEATH, HUD_MK_DEFEAT, HUD_MK_BOOKMARK };
typedef struct { float t; int kind; int seat; } HUDMARK;   /* seat < 0: a bookmark */

/* The foot plate's toggles, one bit each, named as layout.json's `toggle`. */
#define HUD_TOG_STATS       1u
#define HUD_TOG_CURSORS     2u
#define HUD_TOG_CLICKS      4u
#define HUD_TOG_SELECTIONS  8u
#define HUD_TOG_CHAT        16u
unsigned tagpu_hudlay_toggle(const char* name);    /* 0: no such toggle */
const char* tagpu_hudlay_toggle_name(unsigned bit);

#define HUD_TOASTLEN 64

/* A live fork of the playing's file, in seconds from the root's first GameTime, as the mockup's
   forkList() gives it: `flags` the packet's TAGPU_PK_FORK_* (the one watched, a segment of the
   chain that plays). */
typedef struct { int id; char name[32]; float from, end; int parent; int flags; } HUDFORK;

/* THE STATS WINDOW (hud.js `windowHtml`, `chart`): its place, its measure and its split as the
   input keeps them, and the groups the host made of the samples -- one per player, or one per
   team summed -- as index.html's `state()` makes them. */
#define HUD_MAXPT    512        /* a group's samples: TAGPU_PK_HUD_POINTS                     */
#define HUD_MEASURE  12         /* a measure's key, layout.json window.measures `k`            */
typedef struct {
    char     name[32];          /* UTF-8 */
    uint32_t colour;            /* 0xRRGGBB */
    float    death;             /* the commander's death, seconds: a cross on the line; < 0 none */
    float    v[HUD_MAXPT];
} HUDGROUP;
typedef struct {
    int      open;
    float    x, y;              /* the window's top left as asked; the build keeps it on screen */
    char     measure[HUD_MEASURE];
    int      team;              /* the groups are the teams: `teams` is the split pressed       */
    float    dt, t0;            /* sample s is at t0 + s * dt seconds                           */
    int      npt, ngroup;
    HUDGROUP group[HUD_MAXSEAT];
} HUDSTATS;

typedef struct {
    float    W, H;                  /* the screen                                            */
    float    panel, top, bottom;    /* the engine's frame: side panel width, bar heights     */
    float    t, T;                  /* the replay's clock and its length, seconds            */
    int      playing;
    float    speed;                 /* shown as "<speed>×"                                   */
    float    hud;                   /* the replay HUD's scale, shown as a percentage         */
    int      nseat;
    HUDSEAT  seat[HUD_MAXSEAT];
    int      nteam;                 /* one team: no headers                                  */
    int      team_n[HUD_MAXSEAT];
    uint8_t  team_seat[HUD_MAXSEAT][HUD_MAXSEAT];
    unsigned folded;                /* bit t: team t's rows are folded away                  */
    unsigned toggles;               /* HUD_TOG_*                                             */
    int      nmark;
    HUDMARK  mark[HUD_MAXMARK];
    float    mx, my;                /* the pointer, for the hovered row; < 0: none           */
    float    px;                    /* the device pixels a unit: the hint's hit test snaps the
                                       controls' edges at it as the hit map does (hint)      */
    char     held[HUD_KEYLEN];      /* the key of the button held down; "" = none            */
    int      sight, follow;         /* the seat Sight and Follow are set to; < 0: none, the
                                       full map and the free camera                          */
    int      scroll;                /* the column's first item; held to its last in the build */
    char     toast[HUD_TOASTLEN];   /* UTF-8; "" = none                                       */
    HUDSTATS stats;
    /* proposed, variation B (hud.js `dock`, `forksBay`, `lanes`, `takeover`, `picker`) */
    float    ends;                  /* the original's end, seconds: the groove past it is shaded */
    float    len;                   /* the watched chain's end: the time text's second clock  */
    float    fill_to;               /* the track's fill stops here                            */
    int      nfork, fork_max;
    HUDFORK  fork[HUD_MAXFORK];     /* in the mockup's forkList() order                       */
    int      timeline;              /* the watched fork's index in fork[]; -1 the original    */
    int      lanes_open;
    int      picker;                /* the picker is drawn                                    */
    int      pick_down;             /* the Take over button is down: asked, seeking or open   */
    int      replace;               /* the fork a takeover replaces, while the picker is open;
                                       -1 none                                                */
    int      armed;                 /* the fork whose Delete is armed; -1 none                */
    /* live (hud.js `live`): in live mode `t` is the playhead and `T` the strip's end, both in
       seconds from the rewind's first GameTime */
    int      live;                  /* the live HUD is drawn, not the replay's                */
    int      live_open;             /* the strip is drawn: the rewind is open or in the past  */
    int      rw_down;               /* the rewind button is down: a rewind owns the level     */
    int      rw_arm;                /* Play from here's arm, as the packet's                  */
} HUDSTATE;

/* What to draw, in paint order, and where a click lands.
     HOP_LINE   a straight stroke with butt ends, from (r.x, r.y) to (r.x + r.w, r.y + r.h), `size`
                units wide: an SVG <line>;
     HOP_CHART  the chart's lines (HUDOPS `chart`) in the box r, every group composited in order
                and the whole at the op's opacity: an SVG <g opacity> of polylines. */
enum { HOP_PART, HOP_SOLID, HOP_DOT, HOP_TEXT, HOP_HOT, HOP_LINE, HOP_CHART };
#define HOPF_DIM    1u      /* the dim filter of a player who is out (layout.json `dim`) */
#define HOPF_SHADE  2u      /* HOP_SOLID: an inner shadow `shade` units deep, black 55 % */
#define HOPF_RING   4u      /* HOP_SOLID: an inset edge `size` units deep inside r, drawn as
                               four solids between r and r inset by `size`, every edge snapped
                               once and shared by the sides that meet on it (tagpu_huddraw.c).
                               HOP_DOT: the band `size` units deep inside the circle inscribed in
                               r, unsnapped as a canvas arc is -- a canvas arc's stroke, or, as
                               deep as the radius, its fill (tagpu_wikistage.c) */
#define HOPF_DROP   8u      /* HOP_SOLID: CSS box-shadow 0 `size` `shade` round the box r, in the
                               op's colour, painted outside r only */
#define HOPF_BASE   16u     /* HOP_TEXT: an SVG <text>, r.x its anchor and r.y its baseline;
                               `centre` is its text-anchor, 0 start, 1 middle, 2 end */
#define HOPF_TOP    32u     /* an op of the top layer, drawn after the engine's UI */
#define HOPF_INK    64u     /* HOP_TEXT: in its runs' colour with neither the style's shadow nor
                               its outline: the dark ink of text on a light face (tagpu_wikilay.c) */
#define HOPF_SPRITE 128u    /* HOP_PART: not a kit part but cell `part` of the sprite sheet
                               (HUD_TEX_SPRITE), its top-left `size` x `shade` texels drawn whole
                               into r, sampled nearest: a game cursor (tagpu_wikistage.c) */

/* THE SPRITE SHEET: HUD_SPRITE_CELLS cells of HUD_SPRITE_CELL x HUD_SPRITE_CELL RGBA texels side by
   side, a frame in the top left of its cell. TA's largest cursor frame is 52 x 53. */
#define HUD_SPRITE_CELL   64
#define HUD_SPRITE_CELLS  16

typedef struct {
    uint8_t  kind, flags;
    int16_t  part;          /* HOP_PART: the kit index                                    */
    HUDRECT  r;
    HUDRECT  clip;          /* w < 0: none                                                */
    uint32_t rgba;          /* HOP_SOLID, HOP_DOT, HOP_LINE: the colour; HOP_PART, HOP_TEXT:
                               0xFFFFFF00 | opacity                                          */
    float    shade;         /* HOPF_SHADE                                                 */
    int16_t  style;         /* HOP_TEXT: the style's index in layout.json `styles`        */
    uint8_t  centre;        /* HOP_TEXT: 1 centred, 2 to the box's right, 0 from its left */
    uint8_t  grp;           /* 0, or the group it is drawn in: HUDOPS `grp[grp - 1]`        */
    float    size;          /* HOP_TEXT: the font size in units, after `fit`; HOPF_DROP: the
                               shadow's offset; HOPF_RING: the edge's depth; a line's and the chart's stroke width */
    uint16_t run, nrun;     /* HOP_TEXT: its runs; HOP_HOT: run = the key's offset in text */
} HUDOP;

typedef struct { uint32_t rgb; int resource; uint16_t off, len; } HUDRUN;   /* rgb: 0 = the style's */

/* The chart's lines, in units from the screen's top left, for the HOP_CHART ops that draw them:
   each group a polyline `w` wide, under it a halo `halo_w` wide when its colour is dark, and the
   cross of its commander's death, two strokes `cross_w` wide `cross` from its centre. */
typedef struct {
    int      n, npt;
    float    w, halo_w, cross, cross_w;
    uint32_t halo_rgb;
    float    halo_a;
    struct { uint32_t rgb; int dark, cross; float cx, cy; } g[HUD_MAXSEAT];
    float    pt[HUD_MAXSEAT][HUD_MAXPT][2];
} HUDCHART;

#define HUD_MAXOP   4096
/* THE GROUPS: a disabled control is one layer, as the mockup's CSS `opacity` on its element makes
   it -- its part, icon, stripe and text composited at full opacity, then the whole faded once.
   Its ops are consecutive and carry its number (`grp`); `grp[]` holds each group's opacity.
   Each group stands at a site tagpu_hudlay_max_hot counts -- a control whose hot() it takes the
   place of while disabled -- one group a site, so a frame has at most that many groups, which the
   load holds to HUD_MAXHOT; a group never holds the chart or a drop shadow. */
#define HUD_MAXGRP  HUD_MAXHOT
#define HUD_MAXRUN  2048
#define HUD_TEXTMAX 32768

typedef struct {
    int     n, nrun, ntext;
    int     overflow;       /* something did not fit and was dropped                      */
    int     first, last;    /* the column's first item as drawn, and the greatest it can be:
                               the scroll the state asked for, held to [0, last]           */
    float   track_x0, track_x1; /* the playhead's run along the timeline, start to end     */
    HUDCHART chart;
    int     ngrp;
    float   grp[HUD_MAXGRP];
    HUDOP   op[HUD_MAXOP];
    HUDRUN  run[HUD_MAXRUN];
    char    text[HUD_TEXTMAX];
} HUDOPS;

/* One part of the kit. */
enum { HK_GRID, HK_NINE, HK_TILE, HK_WHOLE, HK_ICON };
#define HUD_MAXBAND 8
typedef struct {
    char        name[32];
    int         kind;
    float       scale;              /* the PNG's pixels per unit                          */
    int         pw, ph;             /* the PNG's size, from kit.json                      */
    float       design[2];          /* HK_GRID                                            */
    int         nxs, nys;
    float       xs[HUD_MAXBAND][2], ys[HUD_MAXBAND][2];
    const TJV*  wells;
    float       slice[4];           /* HK_NINE: top, right, bottom, left, in PNG pixels   */
    int         xtile;              /* HK_GRID, HK_NINE: the x bands repeat, not stretch  */
} HUDPART;

typedef struct {
    float press_shift;
    float win_w, win_h, win_x, win_y;   /* the stats window's size and its first place       */
    float track_ends;               /* the playhead's inset at each end of the track      */
    unsigned toast_ms;              /* how long a toast shows                             */
    float    toast_w, toast_max_w;  /* its box's least and most width, units              */
    float    toast_pad;             /* the room each side of its text, units              */
    /* a button's hint (layout.json `tip`): its box's height, the room each side of its text, its
       gap above the button and its hold inside the screen's edges, units; its fill and edge */
    float    tip_h, tip_pad, tip_gap, tip_margin, tip_alpha, tip_edge;
    uint32_t tip_rgb, tip_edge_rgb;
    int      tip_style;             /* its text's style, an index into `style`            */
    float    live_clear;            /* from the top bar's lower edge to the first row clear of
                                       the live strip, its gap under it as over it: units     */
    float dim_brightness, dim_saturate;
    uint32_t res_rgb[2];            /* metal, energy                                      */
    uint32_t pos_rgb, neg_rgb;
    int      nteam_rgb;
    uint32_t team_rgb[8];           /* the chart's team colours, layout.json chart.teams   */
    int      nstyle;
    struct HUDSTYLE {
        char     name[24];
        char     font[32];
        int      weight;
        float    size;
        uint32_t rgb;
        int      resource;          /* the colour is the line's resource's                 */
        int      upper;
        float    spacing;           /* em                                                 */
        float    shadow;            /* > 0: a drop of that many units, black 80 %         */
        int      outline;
        float    ol[4];             /* px, py, nx, ny                                     */
    } style[24];
} HUDGLOBALS;

typedef struct HUDLAY HUDLAY;

/* The two files' texts, parsed, checked and resolved. NULL with the reason in `err`: a parse
   error, a part `png_size` cannot size, or a field the rules read that is missing or of the
   wrong type, by name. `png_size` gives a part's PNG dimensions, which kit.json does not hold:
   1 on success. Every rule is run once here on a state that takes every branch, so a layout
   that loads cannot fail to build. */
typedef int (*HUDPNGSIZE)(void* ctx, const char* part, int* w, int* h);
HUDLAY*           tagpu_hudlay_load(const char* layout, size_t nlayout, const char* kit, size_t nkit,
                                    HUDPNGSIZE png_size, void* ctx, char* err, size_t errn);
void              tagpu_hudlay_free(HUDLAY* l);
/* A kit with no layout -- the in-game wiki's (tagpu_hudart.c, tagpu_wikilay.c) -- parsed by the
   same part reader: its parts, and, when `styles` is not NULL, the styles and the measure of that
   loaded layout, whose faces draw its text and which must outlive this one. NULL with the reason
   in `err`. Never built with: it has no layout to build, so tagpu_hudlay_build must never be given
   it; tagpu_hudlay_expand, _nparts and _part read only the parts. */
HUDLAY*           tagpu_hudlay_load_kit(const HUDLAY* styles, const char* kit, size_t nkit,
                                        HUDPNGSIZE png_size, void* ctx, char* err, size_t errn);
/* A part's index in the kit, a style's in `styles` (HUDGLOBALS.style), by name; -1 when absent. */
int               tagpu_hudlay_part_index(const HUDLAY* l, const char* name);
int               tagpu_hudlay_style_index(const HUDLAY* l, const char* name);
/* How wide a string is set in style `style` (HUDGLOBALS.style) at `size` units, in units: the
   faces' measure (tagpu_hudtext_width), which the layout itself has no faces for. A box sized
   to its text -- the toast -- asks it; until it is given one, such a box keeps its least width.
   Set once, before the layout is published to the thread that builds with it. */
typedef double (*HUDTEXTW)(void* ctx, int style, double size, const char* s);
void              tagpu_hudlay_measure(HUDLAY* l, HUDTEXTW measure, void* ctx);
/* The most HOP_HOT ops any state can build with this layout: the sum, over every place the rules
   emit one, of the most that place can emit -- the layout's lists and the code's fixed maxima --
   for the replay's HUD and for the live one, whichever is larger. The load refuses a layout
   whose bound is above HUD_MAXHOT, so every control a loaded layout draws fits the hit map. */
int               tagpu_hudlay_max_hot(const HUDLAY* l);
const HUDGLOBALS* tagpu_hudlay_globals(const HUDLAY* l);
int               tagpu_hudlay_nparts(const HUDLAY* l);
const HUDPART*    tagpu_hudlay_part(const HUDLAY* l, int i);

/* The ops for state `st`, into `out` (about 500 KB: never on a stack). Returns 0 when a field
   the rules read is missing, which tagpu_hudlay_load has already ruled out for every branch. */
int tagpu_hudlay_build(const HUDLAY* l, const HUDSTATE* st, HUDOPS* out, char* err, size_t errn);

/* THE BASELINE OF A TEXT OP, AS CHROME LAYS IT: the line box, the font size tall at line-height 1,
   centred in the op's box `y`, `h`; the face's ascent and descent, `asc` and `desc` in em, each
   rounded to a whole pixel at `size` pixels; the half-leading, here negative, floored to a pixel;
   the baseline the line's top plus the ascent plus that. All in CSS pixels, which in game are the
   device's, and not yet snapped for painting. Pure.

   MEASURED 2026-10-08 (hud-layout.md, "Text"): tools/hudart/laycheck.py checks it against a probe
   on every text's line, and a sweep of the faces' rounded metrics over 1..60 px in 1/64ths matches
   round(em x size) everywhere but at 26 of 7554 Oswald values, where Chrome rounds an ascent or a
   descent that lies just past a half down -- by an arithmetic not identified -- and the line sits
   a pixel off the mockup's. */
float tagpu_hudlay_baseline(float y, float h, float size, float asc, float desc);

/* A part op as textured quads at `px` device pixels a unit, each a destination rectangle in
   device pixels and a source rectangle in the part's texture coordinates (u may run past 1 for
   a tiled fill, which repeats). Returns the count, at most `cap`: a grid part is one quad a
   cell, and its HUD_MAXBAND bands an axis cut that axis into at most 2 * HUD_MAXBAND + 1
   segments, so HUD_MAXQUAD is every cell of the largest grid and a caller that offers that many
   loses none. An `xtile` part's band copies are one quad each, and they are fitted to what is
   left of `cap` after every cell has its one: a band drawn longer than that holds fewer, longer
   copies, never a missing cell. */
#define HUD_MAXQUAD ((2 * HUD_MAXBAND + 1) * (2 * HUD_MAXBAND + 1))
typedef struct { float x0, y0, x1, y1, u0, v0, u1, v1; } HUDQUAD;
int tagpu_hudlay_expand(const HUDLAY* l, const HUDOP* op, float px, HUDQUAD* q, int cap);

/* The op's clip at `px` device pixels a unit, in units: its own, except that a part's clip of
   positive width -- a fill, the track's or a lane's -- spans at least one device pixel, so a fill
   that has begun always shows. The draw clips by pixel centre on a half-open rect, so one device
   pixel holds exactly one centre; hud.js gives a `.clip` box the same floor in CSS. */
HUDRECT tagpu_hudlay_clip(const HUDOP* op, float px);

/* A device coordinate rounded to the whole pixel an edge is drawn on: half up, as Chrome snaps an
   element, with an edge less than a sixty-fourth of a pixel below a half counted as on it. The
   layout computes in float and hud.js in double, and an edge that falls exactly on a half -- at
   1.5 px a unit, any odd unit; at 4.5, a third of one -- comes out a hair either side of it by their
   arithmetic: float's 24 bits leave under 2.5e-4 px a rounding below 4096 px, a few roundings an
   edge [INFERRED from the format], and laycheck finds the two sides' edges equal on every box. The
   margin makes both sides round such an edge up. hud.js pins its boxes and composes its parts by the
   same rule (snapPx).
   ONE ARITHMETIC WHEREVER IT RUNS: the parameter is a float, so every caller rounds its product
   to float first -- a sum inside it is rounded to float by the caller too (tagpu_hud_edge) -- and
   the rest is in double, where adding the half and the margin to a float is exact. The DLL's x87
   code (-std=c99 rounds at an assignment, a cast and a parameter: -fexcess-precision=standard)
   and laycheck's SSE build therefore give the same pixel for the same float. The sums are exact at
   53-bit x87 precision, Windows' default for a thread, which the DLL never changes. */
static inline double tagpu_hud_snap(float v) { return floor((double)v + 0.5 + 1.0 / 64); }

/* the far edge of a box from `a` units long `b`, at `px` a unit, snapped: the sum and the product
   each rounded to float, as every caller computes them */
static inline double tagpu_hud_edge(float a, float b, float px)
{
    float e = a + b;
    return tagpu_hud_snap(e * px);
}

/* a ring's (HOPF_RING) edges in device pixels: X[0], X[3] the outer box's, X[1], X[2] the inner
   box's, Y the same. Each is snapped once and the four sides share them -- top X0..X3 x Y0..Y1,
   bottom X0..X3 x Y2..Y3, left X0..X1 x Y1..Y2, right X2..X3 x Y1..Y2 -- so they meet with neither
   a gap nor an overlap. An edge deeper than half the box meets its opposite in the middle. hud.js
   `pinRing` computes the same edges. */
static inline void tagpu_hud_ring(const HUDOP* op, float px, float X[4], float Y[4])
{
    X[0] = (float)tagpu_hud_snap(op->r.x * px);
    Y[0] = (float)tagpu_hud_snap(op->r.y * px);
    X[3] = (float)tagpu_hud_edge(op->r.x, op->r.w, px);
    Y[3] = (float)tagpu_hud_edge(op->r.y, op->r.h, px);
    X[1] = (float)tagpu_hud_edge(op->r.x, op->size, px);
    Y[1] = (float)tagpu_hud_edge(op->r.y, op->size, px);
    X[2] = (float)tagpu_hud_edge(op->r.x + op->r.w, -op->size, px);
    Y[2] = (float)tagpu_hud_edge(op->r.y + op->r.h, -op->size, px);
    if (X[1] > X[2]) X[1] = X[2] = (float)floor((X[1] + X[2]) / 2);
    if (Y[1] > Y[2]) Y[1] = Y[2] = (float)floor((Y[1] + Y[2]) / 2);
}

/* Whether the state's teams are told apart: two or more, one of them with two players (hud.js,
   `headed`). The column's headers, the teams chip and the stats window's team split follow it. */
int tagpu_hudlay_headed(const HUDSTATE* st);

#endif
