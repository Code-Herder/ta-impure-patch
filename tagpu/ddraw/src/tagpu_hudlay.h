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
    float    pick_t;                /* the picker's title clock, seconds: R                   */
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
#define HOPF_EDGE   4u      /* HOP_SOLID: one side of an inset edge, the mockup's box-shadow */
#define HOPF_DROP   8u      /* HOP_SOLID: CSS box-shadow 0 `size` `shade` round the box r, in the
                               op's colour, painted outside r only */
#define HOPF_BASE   16u     /* HOP_TEXT: an SVG <text>, r.x its anchor and r.y its baseline;
                               `centre` is its text-anchor, 0 start, 1 middle, 2 end */
#define HOPF_TOP    32u     /* an op of the top layer, drawn after the engine's UI */

typedef struct {
    uint8_t  kind, flags;
    int16_t  part;          /* HOP_PART: the kit index                                    */
    HUDRECT  r;
    HUDRECT  clip;          /* w < 0: none                                                */
    uint32_t rgba;          /* HOP_SOLID, HOP_DOT: the colour; HOP_PART: 0xFFFFFF00 | opacity */
    float    shade;         /* HOPF_SHADE                                                 */
    int16_t  style;         /* HOP_TEXT: the style's index in layout.json `styles`        */
    uint8_t  centre;        /* HOP_TEXT: centred, else from the box's left                */
    float    size;          /* HOP_TEXT: the font size in units, after `fit`              */
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
#define HUD_MAXRUN  2048
#define HUD_TEXTMAX 32768

typedef struct {
    int     n, nrun, ntext;
    int     overflow;       /* something did not fit and was dropped                      */
    int     first, last;    /* the column's first item as drawn, and the greatest it can be:
                               the scroll the state asked for, held to [0, last]           */
    float   track_x0, track_x1; /* the playhead's run along the timeline, start to end     */
    HUDCHART chart;
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
/* The most HOP_HOT ops any state can build with this layout: the sum, over every place the rules
   emit one, of the most that place can emit -- the layout's lists and the code's fixed maxima.
   The load refuses a layout whose bound is above HUD_MAXHOT, so every control a loaded layout
   draws fits the hit map. */
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

/* Whether the state's teams are told apart: two or more, one of them with two players (hud.js,
   `headed`). The column's headers, the teams chip and the stats window's team split follow it. */
int tagpu_hudlay_headed(const HUDSTATE* st);

#endif
