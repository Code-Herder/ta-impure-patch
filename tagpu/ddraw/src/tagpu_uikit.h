#ifndef TAGPU_UIKIT_H
#define TAGPU_UIKIT_H
/* tagpu_uikit -- the widgets of Impure's own full-colour windows: boxes in the browser's rounding,
   parts, solids, the hit map, wrapped text, scrolling panels, rows, buttons and badges. A window
   (tagpu_wikilay.c) is a pure layout over these; its mockup draws with tools/uikit/uikit.js.

   A PURE LIBRARY, like tagpu_hudlay.c: it reads what it is given and writes the HUDOPS it is given,
   and nothing else -- no engine memory, no global, no clock. Pure C99 and the C library, so a
   window's check compiles it natively. A UI is one thread's, for one build.

   The widgets take their numbers from a window's layout, in units (the screen's height / 720); the
   lengths they work in are CSS pixels, which in the game are the device's. Ops come out in units. */
#include <stddef.h>
#include <stdint.h>
#include "stb_truetype.h"
#include "tagpu_hudlay.h"
#include "tagpu_json.h"

#define UI_MAXPART  128     /* the parts a window's lightness table holds: tagpu_hudlay.c's HUD_MAXPART */
#define UI_MAXGLYPH 2048    /* the code points of one text the rules break; the rest of a longer one
                               is not set                                                          */
#define UI_MAXLINE  256     /* the lines of one text; the rest is not set                          */
#define UI_LOCK_RGB  0xD01818u  /* a lock disc's fill ...                                           */
#define UI_LOCK_EDGE 0x5A0808u  /* ... and its 1 px border                                          */

typedef struct { double x, y, w, h; } UIR;      /* CSS pixels */

/* A text style's face (art/hud/layout.json `styles`, by index): tagpu_hudart.c's HUDFACE. */
typedef struct {
    const stbtt_fontinfo* info;     /* NULL: the style's text measures nothing                  */
    float                 ascent;   /* em, descent positive, as the browser lays a line out by  */
    float                 descent;
} UIFACE;

/* The faces, by style index, and two answers the browser gives otherwise than the faces do. Both
   are NULL in the game, which sets every text in its own faces at its own size; a check gives the
   browser's, so the layout's rules are compared on what the mockup sets, and the two gaps are
   named (research/notes/ingame-wiki.md, "Gaps"):
     missing  the advance of a code point no face has, which the mockup sets in the browser's
              fallback face and the DLL does not carry; NULL measures it as its face's .notdef, as
              the game draws it. The check fails on any text that needs it: the control glyphs and
              the tree's marks, which no face has, are kit icons.
     size     the size a face is set at for a font size, both in pixels. Chrome sets a face at a
              size of whole sixty-fourths of a pixel near the CSS one, by a rule not identified
              (MEASURED 2026-10-25: 9.0667 px is set at 579/64, 9.07 at 580/64, 10.6667 at
              682/64, 12.75 at 816/64), so at a font size that is no whole number of
              sixty-fourths its lines are up to 0.2 % narrower than the faces'; NULL sets it at
              the size. */
typedef struct {
    UIFACE face[24];
    double (*missing)(void* ctx, int style, double size, uint32_t cp);   /* units at `size` units */
    double (*size)(void* ctx, int style, double px);
    void*    ctx;
} UIFACES;

/* One build's context. ui_begin sets the fields above `cp`; the fields from `cp` on are a text's
   working space, whose last ui_pens and ui_wrap a window may read. */
typedef struct {
    HUDOPS*                o;
    const UIFACES*         f;
    const struct HUDSTYLE* style;       /* by style index, up to the 24 of HUDGLOBALS              */
    const unsigned char*   light;       /* UI_MAXPART: a part's face is light (ui_face_light)      */
    double                 px;          /* pixels a unit                                            */
    int                    maxhot;      /* the hit map's bound: the window's                        */
    int                    nhot;
    int                    top;         /* every op is the top layer's (HOPF_TOP)                   */
    uint8_t                grp;
    /* one text's code points, their byte offsets and the pen before each, in pixels */
    uint32_t               cp[UI_MAXGLYPH + 1];
    int                    off[UI_MAXGLYPH + 1];
    float                  pen[UI_MAXGLYPH + 1];
    int                    ls[UI_MAXLINE], le[UI_MAXLINE];  /* a wrapped text's lines, as code point ranges */
} UI;

/* `style` and `light` are tables of the window's, immutable while it draws. */
void   ui_begin(UI* u, HUDOPS* o, const UIFACES* f, const struct HUDSTYLE* style, const unsigned char* light,
                double px, int maxhot);

/* ---- Chrome's rounding ---- */

double ui_lu(double v);             /* a length to LayoutUnit: the float the CSS value is stored as,
                                       truncated to a sixty-fourth                                 */
double ui_half(double v);           /* LayoutUnit / 2, as a flex container centres: integer division
                                       of sixty-fourths                                            */
double ui_ceil64(double v);         /* a text's width as an intrinsic size: rounded up to a
                                       sixty-fourth                                                */
double ui_len(const UI* u, double n);   /* a layout number in units, as a CSS length               */

UIR    ui_rect(double x, double y, double w, double h);

/* ---- the ops ---- */

HUDOP* ui_op(UI* u, int kind, UIR r);
void   ui_clip(const UI* u, HUDOP* p, UIR k);
int    ui_text_put(UI* u, const char* s, size_t n);     /* its offset, -1 with the overflow set    */
HUDOP* ui_art(UI* u, int part, UIR r, const UIR* clip); /* part < 0 draws nothing                  */
void   ui_glyph(UI* u, int icon, UIR b);                /* a kit glyph over its part's box, as large
                                                           as fits in 0.6 of it, centred; -1 none  */
HUDOP* ui_solid(UI* u, int kind, UIR r, uint32_t rgb, double alpha, const UIR* clip);
void   ui_disc(UI* u, UIR b, const UIR* clip);          /* a lock's circle with a 1 px border,
                                                           box-sizing border-box                   */
/* A control, given only where it is on screen: the part of its box inside `clip`. Past `maxhot`,
   the window's bound, it is dropped and the overflow set, visibly. */
void   ui_hot(UI* u, UIR b, const UIR* clip, const char* key);
/* A disabled control's ops, drawn between the two, are one group faded once (tagpu_hudlay.h,
   THE GROUPS). */
void   ui_group_begin(UI* u, double alpha);
void   ui_group_end(UI* u);

/* ---- text ---- */

typedef struct {
    int    style;
    double fs;              /* the font size, pixels: calc(size * --u), a float        */
    double set;             /* the size its face is set at (UIFACES.size)              */
    double L;               /* the line height, pixels                                 */
    int    upper;
    double sp;              /* letter-spacing, pixels                                  */
} UITS;

/* style `style` with lines `line` times its font size */
UITS   ui_ts(const UI* u, int style, double line);
/* `s`'s code points into u->cp and u->off and the pen before each into u->pen, u->pen[n] the
   advance: each glyph's advance and the kerning before it, letter-spacing after every one, as
   tagpu_hudtext_pens sets a line. Returns n. */
int    ui_pens(UI* u, const UITS* t, const char* s, size_t len);
/* the text's intrinsic widths over ui_pens: its whole line (max-content) and its widest unbreakable
   piece (min-content) */
void   ui_intrinsic(const UI* u, int n, double* maxc, double* minc);
/* Greedy lines of width `w` over ui_pens into u->ls, u->le; returns their count, at least one. */
int    ui_wrap(UI* u, int n, double w);
/* One line of text: code points [a, b) of `s` as ui_pens read it, drawn from x, its line box at
   `top`. The op's box is the line's measure across and the font size high, placed so that
   tagpu_hudlay_baseline gives the browser's baseline for a line `t->L` tall: the line's top, the
   face's rounded ascent, and the half-leading floored. */
void   ui_line(UI* u, const UITS* t, const char* s, int a, int b, double x, double top, int ink, const UIR* clip);
/* A block of text `w` wide from (x, y), wrapped: returns its height, the lines times their height.
   Emits only with `emit`. */
double ui_block(UI* u, int style, double lh, const char* s, double x, double y, double w, int ink, const UIR* clip, int emit);
/* text on a part is in dark ink when the part's face is light */
int    ui_light(const UI* u, int part);

/* ---- the panels: a kit panel with its list scrolling inside its frame ---- */

typedef struct {
    double pn[4];           /* geom.kit pn: the scroller's inset from the panel's box, t r b l */
    double pad[2];          /* the scroller's padding, y and x                                 */
    double scrollbar;
} UIPANELG;

typedef struct {
    UIR    box;             /* the panel, its part's box                                   */
    UIR    scr;             /* the scroller: the panel inset by UIPANELG.pn                */
    UIR    view;            /* what shows: the scroller's padding box, less its scrollbar  */
    double cx, cy, cw;      /* the content's left, its top as scrolled, its width          */
    double sb;              /* the scrollbar's thickness, 0 when the content fits          */
    double extent;          /* the scrolled height: the padding and the content            */
    double scroll;          /* as drawn                                                    */
} UIPANEL;

/* What a panel answers, in units: its scroll as drawn and its limit, and its scrollbar as drawn, so
   a press is read against the thumb the player saw (the track's h is 0 when it shows none). */
typedef struct {
    float   scroll, scroll_max;
    HUDRECT track;
    float   thumb_y, thumb_h;
} UISCROLL;

/* A panel's list `w` wide: returns its height. Emits only with `emit`; the rows go at p->cx and
   p->cy, clipped to p->view, and the widths and heights must not depend on `emit`. */
typedef double (*UILIST)(UI* u, const UIPANEL* p, double w, int emit, void* user);

/* A panel at `box` with its list, `want` units scrolled: laid out without a scrollbar, and again
   with one when the list overflows the scroller, as the browser does; then drawn, its part, its
   scroll control `key`, its list and its thumb. */
void   ui_panel(UI* u, const UIPANELG* g, int part, UIR box, float want, UILIST list, void* user, const char* key, UISCROLL* out);

/* ---- the rows: a flex row of a mark, its text and a lock ---- */

typedef struct {
    double h;               /* its least height                     */
    double pad[2];          /* y and x                              */
    double gap;             /* between the mark, the text, the lock */
    double after;           /* below it                             */
    double lock;            /* the lock's side                      */
    double line;
} UIROWG;

typedef struct {
    int         part;       /* -1: none */
    int         style;      /* the text's */
    const char* text;
    int         mark_style; /* a text mark's */
    const char* mark;       /* "" none */
    int         mark_icon;  /* a mark that is a kit icon filling its square box; -1 none */
    double      mark_w;
    int         mark_right; /* the mark's text to its box's right, else centred */
    int         locked;
    const char* key;        /* its control, or NULL */
    int         ink;        /* the panel's ink, for a row with no part */
} UIROW;

/* A row `w` wide at (x, y): returns its height with its gap after it. Emits only with `emit`. */
double ui_row(UI* u, const UIROWG* g, const UIROW* rw, double x, double y, double w, const UIR* clip, int emit);

/* ---- buttons and badges ---- */

typedef struct {
    double h, pad, min;     /* its height, its text's padding, its least width */
    double line;
    int    style;
} UIBUTTONG;

/* A button's width: its text's between its padding and Chrome's default 2 px border, which the
   kit skin makes transparent and keeps, and no less than the layout's least */
double ui_button_w(UI* u, const UIBUTTONG* g, const char* label);
/* A button with part `part`: its text centred, or its kit glyph (`icon`, -1 none) over it with no
   text; a disabled one faded as one layer and given no control. Returns its width. */
double ui_button(UI* u, const UIBUTTONG* g, double x, double y, const char* key, const char* label, int icon, int part, int off);

typedef struct {
    double h, pad, gap, dot;
    double line;
    int    style;
} UIBADGEG;

/* A badge's width: as wide as its content -- padding, the dot, the gap, the text */
double ui_badge_w(UI* u, const UIBADGEG* g, const char* text);
/* A badge with part `part` at (x, y): the dot and the text centred in its height. Returns its width. */
double ui_badge(UI* u, const UIBADGEG* g, int part, double x, double y, const char* text);

/* ---- parts and layouts ---- */

/* The kit's part named `name`, -1 when it has none. */
int    ui_find_part(const HUDLAY* k, const char* name);

/* THE FACE LIGHTNESS RULE (tools/uikit/uikit.js uiFaceLight): a part's face is light when its
   middle, inside its slices, scaled to 8 x 8 and its opaque pixels (alpha above 128) averaged at
   0.3 R + 0.59 G + 0.11 B, is above a grey of 120; text on a light face is drawn in dark ink with
   no shadow. Here each of the 64 cells is the box average of the source pixels it covers, the
   canvas's scaling approximated. `rgba` is w x h x 4 bytes, premultiplied by its alpha when
   `premultiplied`. */
int    ui_face_light(const unsigned char* rgba, int w, int h, const float slice[4], int premultiplied);

/* Reading a window's layout.json: a number at L.a, L.a.b or either's [i] (i >= 0; b NULL for none),
   0 when it is missing, with `bad` counted and the first failure in `err` under the name `what`. */
typedef struct { const TJV* L; int bad; char* err; size_t errn; const char* what; } UILAYOUT;
double ui_num(UILAYOUT* x, const char* a, const char* b, int i);

#endif
