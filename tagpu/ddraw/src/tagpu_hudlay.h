#ifndef TAGPU_HUDLAY_H
#define TAGPU_HUDLAY_H
/* tagpu_hudlay -- the replay HUD's layout rules: tools/hudart/mockup/hud.js, in C, over the same
   art/hud/layout.json and art/hud/kit.json. research/notes/hud-layout.md states the rules both
   implement; tools/hudart/laycheck.sh compares this file's rectangles with the mockup's.

   A PURE FUNCTION OF ITS INPUTS. tagpu_hudlay_build reads the loaded files and the state it is
   given, and nothing else: no engine memory, no global, no clock. So the render thread can call
   it to draw and the game thread to hit-test a click, each on its own copy of the state, and
   both get the same rectangles for the same state. A HUDLAY is immutable once loaded and is
   never freed while the HUD can draw (tagpu_json.h says why that is enough).

   Pure C99 and the C library, like tagpu_json.c, so tools/hudart/ compiles it natively.

   Every coordinate is in HUD units, from the screen's top left. */
#include <stdint.h>
#include "tagpu_json.h"

#define HUD_MAXSEAT  16
#define HUD_MAXMARK  256
#define HUD_KEYLEN   24

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

#define HUD_TOG_STATS 1u
#define HUD_TOG_MAP   2u

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
} HUDSTATE;

/* What to draw, in paint order, and where a click lands. */
enum { HOP_PART, HOP_SOLID, HOP_DOT, HOP_TEXT, HOP_HOT };
#define HOPF_DIM    1u      /* the dim filter of a player who is out (layout.json `dim`) */
#define HOPF_SHADE  2u      /* HOP_SOLID: an inner shadow `shade` units deep, black 55 % */

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

#define HUD_MAXOP   4096
#define HUD_MAXRUN  2048
#define HUD_TEXTMAX 32768

typedef struct {
    int     n, nrun, ntext;
    int     overflow;       /* something did not fit and was dropped                      */
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
} HUDPART;

typedef struct {
    float press_shift;
    float track_ends;               /* the playhead's inset at each end of the track      */
    float dim_brightness, dim_saturate;
    uint32_t res_rgb[2];            /* metal, energy                                      */
    uint32_t pos_rgb, neg_rgb;
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
const HUDGLOBALS* tagpu_hudlay_globals(const HUDLAY* l);
int               tagpu_hudlay_nparts(const HUDLAY* l);
const HUDPART*    tagpu_hudlay_part(const HUDLAY* l, int i);

/* The ops for state `st`, into `out` (about 400 KB: never on a stack). Returns 0 when a field
   the rules read is missing, which tagpu_hudlay_load has already ruled out for every branch. */
int tagpu_hudlay_build(const HUDLAY* l, const HUDSTATE* st, HUDOPS* out, char* err, size_t errn);

/* The topmost HOP_HOT under (x, y), its key into `key`; 0 when none. */
int tagpu_hudlay_hit(const HUDOPS* ops, float x, float y, char* key, size_t keyn);

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
   loses none. */
#define HUD_MAXQUAD ((2 * HUD_MAXBAND + 1) * (2 * HUD_MAXBAND + 1))
typedef struct { float x0, y0, x1, y1, u0, v0, u1, v1; } HUDQUAD;
int tagpu_hudlay_expand(const HUDLAY* l, const HUDOP* op, float px, HUDQUAD* q, int cap);

#endif
