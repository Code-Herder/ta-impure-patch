/* tagpu_wikilay.c -- the in-game wiki's window. Contract: tagpu_wikilay.h.

   EACH FUNCTION BELOW IS ONE OF THE MOCKUP'S RULES, IN ITS ORDER: a box of wiki.css with the
   content wiki.js renders into it, emitted in the document's order, which is the order the
   browser paints them. A rule changed in one place is changed in the other; wikilaycheck.py is
   what notices when they disagree.

   THE ARITHMETIC IS CHROME'S. Lengths are worked in CSS pixels, which in the game are the
   device's, because that is the grid Chrome rounds on: every length of the CSS -- calc(n * --u),
   a percentage, a line height -- truncated to a sixty-fourth of a pixel (LayoutUnit's float
   constructor), the space a flex container centres an item in halved by integer division of
   sixty-fourths, a text's intrinsic width rounded up to one, a scrollbar's thickness to a whole
   pixel, a face's ascent and descent to whole pixels and a line's half-leading floored to one.
   Rounded otherwise, the rows of a list drift by a sixty-fourth a row. The ops are converted to
   units at the end.

   THE LINE RULE is the browser's for these texts: greedy, breaking after a run of spaces and
   after a hyphen between a letter and anything but a digit (UAX #14's HY class, which "right-click"
   and "Shift-drag" in today's entries meet), a line's trailing spaces hanging; a word wider than
   its box overflows it. Each line is placed by tagpu_hudlay_baseline's rule with the line's own
   height: its baseline the line's top, the face's rounded ascent and the half-leading floored. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_wikilay.h"
#include "tagpu_wikitext.h"
#include "tagpu_hudtext.h"
#include "tagpu_json.h"
#include "tagpu_mod.h"

/* the kit's parts the rules draw, by name; the icons are looked up as the mockup's data-glyph
   and data-part list them, the first the kit has. The control glyphs and the tree's marks are
   icons because no HUD face has their characters (tools/wiki/mockup/glyphs.js draws them) */
enum { P_WINDOW, P_CLOSE, P_CLOSE_DOWN, P_PANEL, P_ROW, P_ROW_SEL, P_BEZEL, P_BUTTON, P_BUTTON_DOWN,
       P_BADGE, P_KEYCAP, P_WIKIBTN, P_WIKIBTN_DOWN, P_N };
static const char* const PART[P_N] = { "window", "close", "close_down", "panel", "row", "row_selected",
    "bezel", "button", "button_down", "badge", "keycap", "wikibtn", "wikibtn_down" };

/* layout.json `roles`, the text of each element */
enum { R_WINDOW_TITLE, R_ENTRY_TITLE, R_CATEGORY, R_ENTRY, R_DEMO, R_DEMO_NUM, R_SUBTITLE, R_TEXT,
       R_CAPTION, R_CONTROL, R_BADGE, R_KEYCAP, R_KEYS, R_N };
static const char* const ROLE[R_N] = { "window_title", "entry_title", "category", "entry", "demo",
    "demo_num", "subtitle", "text", "caption", "control", "badge", "keycap", "keys" };

/* The mockup's strings: index.html and wiki.js. wikilaycheck.py compares every text, so one that
   drifts from the mockup's is named there. */
#define S_TITLE     "Wiki"
#define S_CHOOSE    "Choose an entry"
#define S_NODEMOS   "No demos"
#define S_BADGE     "Turned off in this mod"
#define S_X1        "1\xC3\x97"             /* 1× */
#define S_X2        "2\xC3\x97"             /* 2× */
#define S_TRY       "Try it yourself"
#define S_BACK      "Back to demo"
static const char* const KEYCAP[WIKI_NKEYS] = { "Shift", "Alt", "Ctrl", "Z", "X", "Left-click", "Right-click" };

/* the colours the CSS gives outright */
#define INK         0x1D1D1Bu               /* text on a light face (wiki.js paintParts --ink)   */
#define LOCK_RGB    0xD01818u               /* .lock, #badge i: the fill ...                      */
#define LOCK_EDGE   0x5A0808u               /* ... and its 1 px border                            */
#define THUMB_RGB   0x808078u               /* ::-webkit-scrollbar-thumb, at THUMB_A              */
#define THUMB_A     0.7
#define OFF_A       0.45                    /* #ctl button:disabled                               */
#define ICON_F      0.6                     /* a kit glyph over its part (paintParts drawIcon)    */

#define MAXPART     128                     /* tagpu_hudlay.c's HUD_MAXPART                       */
#define MAXGLYPH    2048                    /* the code points of one text the rules break; the
                                               rest of a longer one is not set                    */
#define MAXLINE     256                     /* the lines of one text; the rest is not set         */
#if MAXGLYPH > TAGPU_HUDTEXT_MAX
#error a line of one text must fit the glyphs the HUD draw sets of one op
#endif
#define MAXFILL     (4 * MAXGLYPH + 1)      /* a filled text's bytes: the code points the rules set,
                                               four bytes at most each, and the terminator; a
                                               longer fill is cut at a character (tagpu_wikitext.h) */
enum { FILL_TITLE, FILL_SUB, FILL_ROW, FILL_N };    /* the texts filled at once: the entry's title
                                               and subtitle are measured and then set, and a row,
                                               a paragraph or a demo title is set as it is filled */

/* layout.json's numbers, in units unless named */
typedef struct {
    double btn_side, btn_top, btn_right[2];
    double share, font, line;               /* the window's share (percent), its font, its line height */
    double wt[4], wx[4], wb[4], gap, pn[4], bz[4];   /* geom.kit: wt l r t h, wx r t w h, the quads t r b l */
    double title_pad;
    double col[2];                          /* percent */
    double panel_pad[2];
    double scrollbar;
    double indent, mark;
    double row_h, row_pad[2], row_gap, row_after;
    double lock;
    double demo_num;
    double entry_pad[2], entry_gap, title_line;
    double badge_h, badge_pad, badge_gap, badge_dot;
    double stage_min;
    double caption_h, keys_h, keys_gap;
    double keycap_h, keycap_pad;
    double ctl_gap, ctl_pair, ctl_try, ctl_h, ctl_pad, ctl_min;
    double text_share, text_para;
} WG;

struct WIKILAY {
    HUDLAY*          kit;
    WG               g;
    int              part[P_N];
    int              icon_close, icon_wiki;     /* -1: the kit has none */
    int              icon_play, icon_pause, icon_replay, icon_open, icon_shut, icon_dot;
    int              role[R_N];                 /* a style index */
    int              nstyle;
    struct HUDSTYLE  style[24];
    unsigned char    light[MAXPART];
};

typedef struct { double x, y, w, h; } R;        /* CSS pixels */

typedef struct {
    const WIKILAY*   l;
    const WIKIDATA*  d;
    const WIKIFACES* f;
    const WIKIPAGE*  st;
    HUDOPS*          o;
    WIKIFRAME*       fr;
    double           px;                        /* pixels a unit */
    uint8_t          grp;
    int              nhot;
    /* one text's code points, their byte offsets and the pen before each, in pixels */
    uint32_t         cp[MAXGLYPH + 1];
    int              off[MAXGLYPH + 1];
    float            pen[MAXGLYPH + 1];
    int              ls[MAXLINE], le[MAXLINE];  /* a wrapped text's lines, as code point ranges */
    char             fill[FILL_N][MAXFILL];     /* the texts filled, by FILL_*                   */
} C;

/* text.js's fill(e, raw): `raw` (NULL reads "") with entry `e`'s words under the page's casting and
   interface type, into fill buffer `slot`. The words point into the entry and the casting, both
   immutable for as long as the build runs (tagpu_wikilay.h, THE TEXT IS FILLED HERE). */
static const char* filled(C* c, int slot, const WIKIENTRY* e, const char* raw)
{
    WIKIWORDS w;
    tagpu_wikitext_words(&w, e, c->st->casting, c->st->iface);
    tagpu_wikitext_fill(raw, &w, c->fill[slot], sizeof c->fill[slot]);
    return c->fill[slot];
}

/* wiki.js `fill(e, t) || id`: a text that fills to nothing shows its id */
static const char* filled_or(C* c, int slot, const WIKIENTRY* e, const char* raw, const char* id)
{
    const char* t = filled(c, slot, e, raw);
    return *t ? t : id;
}

/* ---- Chrome's rounding ------------------------------------------------------------------------- */

/* a length to LayoutUnit: the float the CSS value is stored as, truncated to a sixty-fourth */
static double lu(double v)
{
    double r = (double)(float)v * 64.0;
    return (r < 0 ? ceil(r) : floor(r)) / 64.0;
}
/* LayoutUnit / 2, as a flex container centres: integer division of sixty-fourths */
static double half(double v)
{
    double r = floor(v * 64.0 + 0.5);
    return (r < 0 ? ceil(r / 2) : floor(r / 2)) / 64.0;
}
/* a text's width as an intrinsic size: rounded up to a sixty-fourth */
static double ceil64(double v) { return ceil(v * 64.0 - 1e-6) / 64.0; }
/* a layout number in units, as a CSS length */
static double U(const C* c, double n) { return lu(n * c->px); }

static R rect(double x, double y, double w, double h) { R r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }
static R inter(R a, R b)
{
    double x0 = fmax(a.x, b.x), y0 = fmax(a.y, b.y), x1 = fmin(a.x + a.w, b.x + b.w), y1 = fmin(a.y + a.h, b.y + b.h);
    return rect(x0, y0, fmax(0, x1 - x0), fmax(0, y1 - y0));
}
static int meets(R a, R clip) { return a.y < clip.y + clip.h && a.y + a.h > clip.y && a.w > 0 && clip.h > 0; }

/* ---- the ops ----------------------------------------------------------------------------------- */

static HUDOP* op(C* c, int kind, R r)
{
    HUDOP* p;
    if (c->o->n >= HUD_MAXOP) { c->o->overflow = 1; return NULL; }
    p = &c->o->op[c->o->n++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->grp = c->grp;
    p->part = -1;
    p->r.x = (float)(r.x / c->px); p->r.y = (float)(r.y / c->px);
    p->r.w = (float)(r.w / c->px); p->r.h = (float)(r.h / c->px);
    p->clip.w = -1;
    p->rgba = 0xFFFFFFFFu;
    return p;
}
static void clip_to(C* c, HUDOP* p, R k)
{
    if (!p) return;
    p->clip.x = (float)(k.x / c->px); p->clip.y = (float)(k.y / c->px);
    p->clip.w = (float)(k.w / c->px); p->clip.h = (float)(k.h / c->px);
}
static int text_put(C* c, const char* s, size_t n)
{
    int at = c->o->ntext;
    if ((size_t)at + n + 1 > HUD_TEXTMAX) { c->o->overflow = 1; return -1; }
    memcpy(c->o->text + at, s, n);
    c->o->text[at + n] = 0;
    c->o->ntext += (int)n + 1;
    return at;
}
static HUDOP* art(C* c, int part, R r, const R* clip)
{
    HUDOP* p;
    if (part < 0 || !(p = op(c, HOP_PART, r))) return NULL;
    p->part = (int16_t)part;
    p->rgba = 0xFFFFFFFFu;
    if (clip) clip_to(c, p, *clip);
    return p;
}
/* a kit glyph over its part's box, as large as fits in ICON_F of it, centred */
static void glyph(C* c, int icon, R b)
{
    if (icon >= 0) art(c, icon, rect(b.x + b.w * (1 - ICON_F) / 2, b.y + b.h * (1 - ICON_F) / 2, b.w * ICON_F, b.h * ICON_F), NULL);
}
static HUDOP* solid(C* c, int kind, R r, uint32_t rgb, double alpha, const R* clip)
{
    HUDOP* p = op(c, kind, r);
    if (p) p->rgba = (rgb << 8) | (uint32_t)lround(255.0 * alpha);
    if (clip) clip_to(c, p, *clip);
    return p;
}
/* a circle with a 1 px border, box-sizing border-box (.lock, #badge i) */
static void disc(C* c, R b, const R* clip)
{
    solid(c, HOP_DOT, b, LOCK_EDGE, 1, clip);
    solid(c, HOP_DOT, rect(b.x + 1, b.y + 1, b.w - 2, b.h - 2), LOCK_RGB, 1, clip);
}
/* A control, given only where it is on screen: the part of its box inside `clip`. The bound: H is
   720 units, so a list's scroller is under 720 units tall, and a row with its gap after it is at
   least layout.json's row.h + row.after, 17.5 today, which puts at most 42 rows of each list on
   screen -- 84 -- beside the window's own 11 controls and the button: under WIKI_MAXHOT, so no
   control on screen goes without its hit. A layout that broke the bound drops the rest and sets
   the overflow, visibly. */
static void hot(C* c, R b, const R* clip, const char* key)
{
    HUDOP* p;
    int at;
    if (clip) b = inter(b, *clip);
    if (b.w <= 0 || b.h <= 0) return;
    if (c->nhot >= WIKI_MAXHOT) { c->o->overflow = 1; return; }
    if (!(p = op(c, HOP_HOT, b))) return;
    if ((at = text_put(c, key, strlen(key))) < 0) { c->o->n--; return; }
    p->run = (uint16_t)at;
    c->nhot++;
}
/* A disabled control's ops, drawn between the two, are one group faded once (tagpu_hudlay.h,
   THE GROUPS). */
static void group_begin(C* c, double a)
{
    if (c->o->ngrp >= HUD_MAXGRP) { c->o->overflow = 1; return; }
    c->o->grp[c->o->ngrp++] = (float)a;
    c->grp = (uint8_t)c->o->ngrp;
}
static void group_end(C* c) { c->grp = 0; }

/* ---- text -------------------------------------------------------------------------------------- */

typedef struct {
    int    style;
    double fs;              /* the font size, pixels: calc(size * --u), a float */
    double set;             /* the size its face is set at (WIKIFACES.size)            */
    double L;               /* the line height, pixels */
    int    upper;
    double sp;              /* letter-spacing, pixels */
} TS;

static TS ts(const C* c, int role, double line)
{
    const struct HUDSTYLE* s = &c->l->style[c->l->role[role]];
    TS t;
    t.style = c->l->role[role];
    t.fs = (double)(float)(s->size * c->px);
    t.set = c->f->size ? c->f->size(c->f->ctx, t.style, t.fs) : t.fs;
    /* the line height is the multiple of the font size rounded to a sixty-fourth (MEASURED
       2026-10-25: at 1024 x 768 a 10.133 px name's rows are 876/64 px, which 1.35 x 10.133
       truncated would make 875/64) */
    t.L = lu(line * (floor(t.fs * 64 + 0.5) / 64));
    t.upper = s->upper;
    t.sp = (double)(float)(s->spacing * t.fs);
    return t;
}

/* `s`'s code points into c->cp and c->off and the pen before each into c->pen, c->pen[n] the
   advance: each glyph's advance and the kerning before it, letter-spacing after every one, as
   tagpu_hudtext_pens sets a line. Returns n. */
static int pens(C* c, const TS* t, const char* s, size_t len)
{
    const WIKIFACE* fc = &c->f->face[t->style];
    const unsigned char *p = (const unsigned char*)s, *e = p + len;
    float sc = fc->info ? stbtt_ScaleForMappingEmToPixels(fc->info, (float)t->set) : 0, w = 0;
    int n = 0, prev = 0;
    while (p < e && n < MAXGLYPH) {
        uint32_t ch;
        int gi;
        c->off[n] = (int)(p - (const unsigned char*)s);
        ch = tagpu_hudtext_utf8(&p, e);
        c->cp[n] = ch;
        if (t->upper) ch = tagpu_hudtext_upper(ch);
        gi = fc->info ? stbtt_FindGlyphIndex(fc->info, (int)ch) : 0;
        if (!gi && c->f->missing) {
            c->pen[n] = w;
            w += (float)(c->f->missing(c->f->ctx, t->style, t->set / c->px, ch) * c->px) + (float)t->sp;
            prev = 0;
        } else if (fc->info) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(fc->info, gi, &adv, &lsb);
            if (prev) w += sc * (float)stbtt_GetGlyphKernAdvance(fc->info, prev, gi);
            c->pen[n] = w;
            w += sc * (float)adv + (float)t->sp;
            prev = gi;
        } else
            c->pen[n] = w;
        n++;
    }
    c->off[n] = (int)(p - (const unsigned char*)s);
    c->cp[n] = 0;
    c->pen[n] = w;
    return n;
}

/* where a line may begin: after a run of spaces, and after a hyphen that follows a letter and
   comes before anything but a digit */
static int may_break(const C* c, int i)
{
    if (i <= 0) return 0;
    if (c->cp[i] == ' ') return 0;
    if (c->cp[i - 1] == ' ') return 1;
    return c->cp[i - 1] == '-' && i >= 2 && c->cp[i - 2] != ' ' && !(c->cp[i] >= '0' && c->cp[i] <= '9');
}
static int trim(const C* c, int a, int b)
{
    while (b > a && c->cp[b - 1] == ' ') b--;
    return b;
}
/* the text's intrinsic widths over pens(): its whole line (max-content) and its widest
   unbreakable piece (min-content) */
static void intrinsic(const C* c, int n, double* maxc, double* minc)
{
    int a = 0, i;
    *minc = 0;
    for (i = 1; i <= n; i++)
        if (i == n || may_break(c, i)) {
            int b = trim(c, a, i);
            *minc = fmax(*minc, c->pen[b] - c->pen[a]);
            a = i;
        }
    *maxc = ceil64(c->pen[trim(c, 0, n)]);
    *minc = ceil64(*minc);
}
/* Greedy lines of width `w` over pens() into c->ls, c->le; returns their count, at least one. */
static int wrap(C* c, int n, double w)
{
    int nl = 0, a = 0;
    while (a < n && c->cp[a] == ' ') a++;
    if (a >= n) { c->ls[0] = c->le[0] = a; return 1; }
    while (a < n && nl < MAXLINE) {
        int b = -1, i;
        for (i = a + 1; i <= n; i++) {
            if (i < n && !may_break(c, i)) continue;
            if (b >= 0 && c->pen[trim(c, a, i)] - c->pen[a] > w) break;
            b = i;
            if (c->pen[trim(c, a, i)] - c->pen[a] > w) break;
        }
        c->ls[nl] = a;
        c->le[nl] = trim(c, a, b);
        nl++;
        a = b;
        while (a < n && c->cp[a] == ' ') a++;
    }
    return nl;
}

/* One line of text: code points [a, b) of `s` as pens() read it, drawn from x, its line box
   at `top`. The op's box is the line's measure across and the font size high, placed so that
   tagpu_hudlay_baseline gives the browser's baseline for a line `t->L` tall: the line's top, the
   face's rounded ascent, and the half-leading floored. */
static void line(C* c, const TS* t, const char* s, int a, int b, double x, double top, int ink, const R* clip)
{
    const WIKIFACE* fc = &c->f->face[t->style];
    const float fs = (float)t->fs;
    const float A = floorf(fc->ascent * fs + 0.5f), D = floorf(fc->descent * fs + 0.5f);
    double base = top + A + floor((t->L - A - D) / 2);
    double w = c->pen[b] - c->pen[a];
    HUDOP* p;
    HUDRUN* r;
    int off;
    if (b <= a) return;
    if (clip && !meets(rect(x, base - A, w, A + D), *clip)) return;     /* its glyphs' box, as the browser clips */
    if (c->o->nrun >= HUD_MAXRUN) { c->o->overflow = 1; return; }
    if (!(p = op(c, HOP_TEXT, rect(x, base - A - floor((fs - A - D) / 2), w, fs)))) return;
    if ((off = text_put(c, s + c->off[a], (size_t)(c->off[b] - c->off[a]))) < 0) { c->o->n--; return; }
    p->style = (int16_t)t->style;
    p->size = (float)(t->fs / c->px);
    if (ink) p->flags |= HOPF_INK;
    r = &c->o->run[c->o->nrun++];
    r->rgb = ink ? INK : 0;
    r->resource = 0;
    r->off = (uint16_t)off;
    r->len = (uint16_t)(c->off[b] - c->off[a]);
    p->run = (uint16_t)(c->o->nrun - 1);
    p->nrun = 1;
    if (clip) clip_to(c, p, *clip);
    else p->clip.w = -1;
}

/* A block of text `w` wide from (x, y), wrapped: returns its height, the lines times their height.
   Emits only with `emit`. */
static double block(C* c, int role, double lh, const char* s, double x, double y, double w, int ink, const R* clip, int emit)
{
    TS t = ts(c, role, lh);
    int n = pens(c, &t, s, strlen(s)), nl, i;
    if (!n) return 0;
    nl = wrap(c, n, w);
    for (i = 0; emit && i < nl; i++) line(c, &t, s, c->ls[i], c->le[i], x, y + i * t.L, ink, clip);
    return nl * t.L;
}
/* text on a part is in dark ink when the part's face is light (wiki.js paintParts) */
static int light(const C* c, int part) { return part >= 0 && part < MAXPART && c->l->light[part]; }

/* ---- the panels: a kit panel with its list scrolling inside its frame (.pnl > .scr) ----------- */

typedef struct {
    R      box;             /* the panel, its part's box                                   */
    R      scr;             /* the scroller: the panel inset by geom.kit pn                */
    R      view;            /* what shows: the scroller's padding box, less its scrollbar  */
    double cx, cy, cw;      /* the content's left, its top as scrolled, its width          */
    double sb;              /* the scrollbar's thickness, 0 when the content fits          */
    double extent;          /* the scrolled height: the padding and the content            */
    double scroll;          /* as drawn                                                    */
    int    which;
} PANEL;

static void panel_open(C* c, PANEL* p, int which, R box)
{
    const WG* g = &c->l->g;
    p->which = which;
    p->box = box;
    p->scr = rect(box.x + U(c, g->pn[3]), box.y + U(c, g->pn[0]), box.w - U(c, g->pn[3]) - U(c, g->pn[1]),
                  box.h - U(c, g->pn[0]) - U(c, g->pn[2]));
    p->sb = 0;
    p->scroll = 0;
}
/* the content's width with or without the scrollbar; a custom scrollbar is as thick as its CSS
   width truncated to a whole pixel */
static double panel_width(C* c, const PANEL* p, int bar)
{
    return p->scr.w - 2 * U(c, c->l->g.panel_pad[1]) - (bar ? floor(U(c, c->l->g.scrollbar)) : 0);
}
/* The content measured `content` tall at the width panel_width(bar): the scrollbar shows when it
   overflows, and the scroll is held to the panel's limit. Chrome's limit is the scroll height
   less the client height, each snapped to a whole pixel. */
static void panel_place(C* c, PANEL* p, double content, int bar)
{
    const WG* g = &c->l->g;
    double pt = U(c, g->panel_pad[0]), mx;
    float want = c->st->scroll[p->which];
    p->sb = bar ? floor(U(c, g->scrollbar)) : 0;
    p->extent = pt + content + pt;
    mx = fmax(0, floor(p->extent + 0.5) - floor(p->scr.h + 0.5));
    /* the offset to a sixty-fourth of a pixel: a whole-pixel scroll given in float units comes back
       whole, so the thumb's truncated place does not drop a pixel */
    p->scroll = fmin(fmax(0, floor((double)want * c->px * 64 + 0.5) / 64), mx);
    p->view = rect(p->scr.x, p->scr.y, p->scr.w - p->sb, p->scr.h);
    p->cx = p->scr.x + U(c, g->panel_pad[1]);
    p->cy = p->scr.y + pt - p->scroll;
    p->cw = panel_width(c, p, bar);
    c->fr->scroll[p->which] = (float)(p->scroll / c->px);
    c->fr->scroll_max[p->which] = (float)(mx / c->px);
}
/* The scrollbar's thumb, by Chrome's ScrollbarTheme: its length the visible share of the track,
   rounded, and its place the scroll's share of the track's free length, truncated -- a scroll
   below one pixel shown as one. The track is the scrollbar's whole length (no buttons). */
static void panel_thumb(C* c, const PANEL* p)
{
    double track = floor(p->scr.h + 0.5), total = floor(p->extent + 0.5), vis = track, len, pos;
    if (!p->sb || total <= vis) return;
    len = fmin(track, floor(vis / total * track + 0.5));
    pos = p->scroll * (track - len) / (total - vis);
    pos = pos > 0 && pos < 1 ? 1 : floor(pos);
    solid(c, HOP_SOLID, rect(p->scr.x + p->scr.w - p->sb, floor(p->scr.y + 0.5) + pos, p->sb, len), THUMB_RGB, THUMB_A, NULL);
    c->fr->track[p->which].x = (float)((p->scr.x + p->scr.w - p->sb) / c->px);
    c->fr->track[p->which].y = (float)(floor(p->scr.y + 0.5) / c->px);
    c->fr->track[p->which].w = (float)(p->sb / c->px);
    c->fr->track[p->which].h = (float)(track / c->px);
    c->fr->thumb_y[p->which] = (float)((floor(p->scr.y + 0.5) + pos) / c->px);
    c->fr->thumb_h[p->which] = (float)(len / c->px);
}

/* ---- the rows: .row and #demos li, a flex row of a mark, its text and (a tree's entry) the lock - */

typedef struct {
    int         part;       /* -1: none (li.none) */
    int         role;       /* the text's */
    const char* text;
    int         mark_role;  /* the mark's: a demo's number */
    const char* mark;       /* "" none */
    int         mark_icon;  /* a tree row's mark, a kit icon filling its square box; -1 none */
    double      mark_w;
    int         mark_right; /* the mark's text to its box's right (a demo's number), else centred */
    int         locked;
    const char* key;        /* its control, or NULL */
    int         ink;        /* the panel's ink, for a row with no part */
} ROW;

/* A row `w` wide at (x, y): returns its height with its gap after it. Emits only with `emit`. */
static double row(C* c, const ROW* rw, double x, double y, double w, const R* clip, int emit)
{
    const WG* g = &c->l->g;
    const double py = U(c, g->row_pad[0]), pxx = U(c, g->row_pad[1]), gap = U(c, g->row_gap);
    const double lock = rw->locked ? U(c, g->lock) : 0, inner_w = w - 2 * pxx;
    TS t = ts(c, rw->role, g->line), tm;
    double fixed = (rw->mark_w > 0 ? rw->mark_w + gap : 0) + (rw->locked ? gap + lock : 0), maxc = 0, minc = 0, sw, h, ih, mh = 0, th;
    int n = pens(c, &t, rw->text, strlen(rw->text)), nl = 0, i, ink = rw->part >= 0 ? light(c, rw->part) : rw->ink;
    R b, ty;
    if (rw->mark_icon >= 0) mh = rw->mark_w;
    else if (rw->mark[0] || rw->mark_w > 0) { tm = ts(c, rw->mark_role, g->line); mh = rw->mark[0] ? tm.L : 0; }
    intrinsic(c, n, &maxc, &minc);
    sw = maxc <= inner_w - fixed ? maxc : fmax(inner_w - fixed, minc);
    if (n) nl = wrap(c, n, sw);
    th = nl * t.L;
    h = fmax(U(c, g->row_h), 2 * py + fmax(fmax(th, mh), lock));
    ih = h - 2 * py;
    b = rect(x, y, w, h);
    if (emit && meets(b, *clip)) {
        double mx = x + pxx, tx = mx + (rw->mark_w > 0 ? rw->mark_w + gap : 0);
        art(c, rw->part, b, clip);
        if (rw->key) hot(c, b, clip, rw->key);
        if (rw->mark_icon >= 0) {
            /* the mark's box is as tall as it is wide, centred in the row's height */
            R mb = rect(mx, y + py + half(ih - mh), rw->mark_w, mh);
            if (meets(mb, *clip)) art(c, rw->mark_icon, mb, clip);
        } else if (rw->mark[0]) {
            int m = pens(c, &tm, rw->mark, strlen(rw->mark));
            double mw = c->pen[m];
            double lx = rw->mark_right ? mx + rw->mark_w - ceil64(mw) : mx + half(rw->mark_w - ceil64(mw));
            line(c, &tm, rw->mark, 0, m, lx, y + py + half(ih - tm.L), ink, clip);
        }
        if (n) {
            pens(c, &t, rw->text, strlen(rw->text));
            ty = rect(tx, y + py + half(ih - th), sw, th);
            for (i = 0; i < nl; i++) line(c, &t, rw->text, c->ls[i], c->le[i], ty.x, ty.y + i * t.L, ink, clip);
        }
        if (rw->locked)
            disc(c, rect(fmax(tx + sw + gap, x + w - pxx - lock), y + py + half(ih - lock), lock, lock), clip);
    }
    return h + U(c, g->row_after);
}

/* ---- the tree (renderTree): categories nest to any depth, their leaves entries --------------- */

static int folded(const WIKIPAGE* st, int n) { return n >= 0 && n < WIKI_MAXNODE && (st->fold[n >> 5] >> (n & 31)) & 1u; }

static int locked(const C* c, int e)
{
    const char* m = tagpu_wikilay_mod_name(c->st->mod);
    int i;
    if (e < 0 || e >= c->d->nentry) return 0;
    for (i = 0; i < c->d->entry[e].nlocked; i++)
        if (!strcmp(c->d->entry[e].locked_in[i], m)) return 1;
    return 0;
}

/* the tree's rows `w` wide, in preorder, a folded category's descendants left out: returns their
   height */
static double tree_rows(C* c, const PANEL* p, double w, int emit)
{
    const WG* g = &c->l->g;
    double y = 0;
    int i, shut = -1;
    char key[HUD_KEYLEN];
    for (i = 0; i < c->d->nnode && i < WIKI_MAXNODE; i++) {
        const WIKINODE* nd = &c->d->node[i];
        ROW rw;
        double ind = nd->depth * U(c, g->indent);
        if (shut >= 0 && nd->depth > shut) continue;
        shut = -1;
        memset(&rw, 0, sizeof rw);
        rw.part = c->l->part[P_ROW];
        rw.role = nd->entry >= 0 ? R_ENTRY : R_CATEGORY;
        rw.mark = "";
        rw.mark_w = U(c, g->mark);
        if (nd->entry >= 0) {
            const WIKIENTRY* e = &c->d->entry[nd->entry];
            rw.text = filled_or(c, FILL_ROW, e, e->title, e->id);
            rw.mark_icon = c->l->icon_dot;
            rw.locked = locked(c, nd->entry);
            if (nd->entry == c->st->entry) rw.part = c->l->part[P_ROW_SEL];
            snprintf(key, sizeof key, "entry:%d", i);
        } else {
            rw.text = nd->title ? nd->title : "";
            rw.mark_icon = folded(c->st, i) ? c->l->icon_shut : c->l->icon_open;
            if (folded(c->st, i)) shut = nd->depth;
            snprintf(key, sizeof key, "fold:%d", i);
        }
        rw.key = key;
        y += row(c, &rw, p->cx + ind, p->cy + y, w - ind, &p->view, emit);
    }
    return y;
}

/* ---- the demo list (renderEntry's #demos) ----------------------------------------------------- */

static const WIKIENTRY* entry(const C* c)
{
    return c->st->entry >= 0 && c->st->entry < c->d->nentry ? &c->d->entry[c->st->entry] : NULL;
}
static int has_demo(const C* c)
{
    const WIKIENTRY* e = entry(c);
    return e && c->st->demo >= 0 && c->st->demo < e->ndemo;
}

static double demo_rows(C* c, const PANEL* p, double w, int emit)
{
    const WG* g = &c->l->g;
    const WIKIENTRY* e = entry(c);
    double y = 0;
    int i;
    char key[HUD_KEYLEN], num[16];
    ROW rw;
    if (!e || !e->ndemo) {
        /* li.none: "No demos", or nothing with no entry; no part, the panel's ink */
        memset(&rw, 0, sizeof rw);
        rw.part = -1;
        rw.role = R_DEMO;
        rw.text = e ? S_NODEMOS : "";
        rw.mark = "";
        rw.mark_icon = -1;
        rw.ink = light(c, c->l->part[P_PANEL]);
        return row(c, &rw, p->cx, p->cy, w, &p->view, emit);
    }
    for (i = 0; i < e->ndemo; i++) {
        memset(&rw, 0, sizeof rw);
        rw.part = c->l->part[i == c->st->demo ? P_ROW_SEL : P_ROW];
        rw.role = R_DEMO;
        rw.text = filled_or(c, FILL_ROW, e, e->demo[i].title, e->demo[i].id);
        snprintf(num, sizeof num, "%d", i + 1);
        rw.mark = num;
        rw.mark_role = R_DEMO_NUM;
        rw.mark_icon = -1;
        rw.mark_w = U(c, g->demo_num);
        rw.mark_right = 1;
        snprintf(key, sizeof key, "demo:%d", i);
        rw.key = key;
        y += row(c, &rw, p->cx, p->cy + y, w, &p->view, emit);
    }
    return y;
}

/* ---- the entry's text (#text): its paragraphs, each with its gap after it -------------------- */

static double text_rows(C* c, const PANEL* p, double w, int emit)
{
    const WIKIENTRY* e = entry(c);
    double y = 0;
    int i, ink = light(c, c->l->part[P_PANEL]);
    for (i = 0; e && i < e->ntext; i++) {
        y += block(c, R_TEXT, c->l->g.line, filled(c, FILL_ROW, e, e->text[i]), p->cx, p->cy + y, w, ink, &p->view, emit);
        y += U(c, c->l->g.text_para);
    }
    return y;
}

/* A panel at `box` with its list: laid out without a scrollbar, and again with one when the list
   overflows the scroller, as the browser does; then drawn, its part, its scroll control, its list
   and its thumb. */
typedef double (*LIST)(C* c, const PANEL* p, double w, int emit);
static void panel(C* c, int which, R box, LIST list, const char* key)
{
    PANEL p;
    double h;
    int bar = 0;
    panel_open(c, &p, which, box);
    p.cx = p.cy = 0;
    p.view = p.scr;
    h = list(c, &p, panel_width(c, &p, 0), 0);
    if (2 * U(c, c->l->g.panel_pad[0]) + h > p.scr.h) { bar = 1; h = list(c, &p, panel_width(c, &p, 1), 0); }
    panel_place(c, &p, h, bar);
    art(c, c->l->part[P_PANEL], box, NULL);
    hot(c, p.view, NULL, key);
    list(c, &p, p.cw, 1);
    panel_thumb(c, &p);
}

/* ---- the controls (#ctl): play, replay, 1x and 2x, Try it yourself ----------------------------- */

/* A button: its part, down while on or held; its text centred, or its kit glyph (`icon`, -1 none)
   over it with no text; a disabled one faded as one layer and given no control. Chrome's default
   style gives a button a 2 px border, which the kit skin makes transparent and keeps, so it
   stands inside the box. */
static double button(C* c, double x, double y, const char* key, const char* label, int icon, int on, int off)
{
    const WG* g = &c->l->g;
    TS t = ts(c, R_CONTROL, g->line);
    int n = pens(c, &t, label, strlen(label));
    double tw = ceil64(c->pen[n]), w = fmax(U(c, g->ctl_min), 2 * U(c, g->ctl_pad) + 4 + tw), h = U(c, g->ctl_h);
    int down = on || !strcmp(c->st->held, key), part = c->l->part[down ? P_BUTTON_DOWN : P_BUTTON];
    R b = rect(x, y, w, h);
    if (off) group_begin(c, OFF_A);
    art(c, part, b, NULL);
    if (!off) hot(c, b, NULL, key);
    glyph(c, icon, b);
    line(c, &t, label, 0, n, x + 2 + U(c, g->ctl_pad) + half(w - 4 - 2 * U(c, g->ctl_pad) - tw), y + 2 + half(h - 4 - t.L),
         light(c, part), NULL);
    if (off) group_end(c);
    return w;
}

/* ---- the entry (#entry): a flex column of title, subtitle, badge, the stage row, caption, keys,
   controls and the text, the stage row taking what the others leave ---------------------------- */

static void entry_col(C* c, R col)
{
    const WG* g = &c->l->g;
    const WIKIENTRY* e = entry(c);
    const double gap = U(c, g->entry_gap), x = col.x + U(c, g->entry_pad[1]), w = col.w - 2 * U(c, g->entry_pad[1]);
    const double top = col.y + U(c, g->entry_pad[0]), ch = col.h - U(c, g->entry_pad[0]);
    const int wink = light(c, c->l->part[P_WINDOW]), off = !has_demo(c);
    const int lk = locked(c, c->st->entry) || c->st->sim_locked;
    const char* title = e ? filled_or(c, FILL_TITLE, e, e->title, e->id) : S_CHOOSE;
    const char* sub = e ? filled(c, FILL_SUB, e, e->subtitle) : "";
    double th, sh, bh, ph, fixed, srow, y, ctl_h = U(c, g->ctl_h), cap_h = U(c, g->caption_h), keys_h = U(c, g->keys_h);
    int items, k;
    R sbox, stage;

    th = block(c, R_ENTRY_TITLE, g->title_line, title, x, 0, w, 0, NULL, 0);
    sh = block(c, R_SUBTITLE, g->line, sub, x, 0, w, 0, NULL, 0);
    bh = lk ? U(c, g->badge_h) : 0;
    ph = lu(ch * g->text_share / 100);
    items = 7 + lk;
    fixed = th + sh + bh + cap_h + keys_h + ctl_h + ph + gap * (items - 1);
    srow = fmax(U(c, g->stage_min), ch - fixed);

    y = top;
    block(c, R_ENTRY_TITLE, g->title_line, title, x, y, w, wink, NULL, 1);
    y += th + gap;
    block(c, R_SUBTITLE, g->line, sub, x, y, w, wink, NULL, 1);
    y += sh + gap;
    if (lk) {
        /* #badge: align-self flex-start, so as wide as its content: padding, the dot, the gap, the text */
        TS t = ts(c, R_BADGE, g->line);
        int n = pens(c, &t, S_BADGE, strlen(S_BADGE));
        double tw = ceil64(c->pen[n]), dot = U(c, g->badge_dot), pad = U(c, g->badge_pad);
        R b = rect(x, y, pad + dot + U(c, g->badge_gap) + tw + pad, bh);
        art(c, c->l->part[P_BADGE], b, NULL);
        disc(c, rect(b.x + pad, y + half(bh - dot), dot, dot), NULL);
        line(c, &t, S_BADGE, 0, n, b.x + pad + dot + U(c, g->badge_gap), y + half(bh - t.L), light(c, c->l->part[P_BADGE]), NULL);
        y += bh + gap;
    }
    /* the stage row: the bezel, the row's full width */
    sbox = rect(x, y, w, srow);
    art(c, c->l->part[P_BEZEL], sbox, NULL);
    stage = rect(sbox.x + U(c, g->bz[3]), sbox.y + U(c, g->bz[0]), lu(sbox.w - (g->bz[3] + g->bz[1]) * c->px),
                 lu(sbox.h - (g->bz[0] + g->bz[2]) * c->px));
    hot(c, stage, NULL, "stage");
    c->fr->stage.x = (float)(stage.x / c->px); c->fr->stage.y = (float)(stage.y / c->px);
    c->fr->stage.w = (float)(stage.w / c->px); c->fr->stage.h = (float)(stage.h / c->px);
    y += srow + gap;
    /* the caption: a fixed height, so a caption appearing cannot move the stage; overflow hidden,
       so a line past the strip is cut (to a sixty-fourth of a pixel, Chrome's LayoutUnit) */
    {
        R cap = rect(x, y, w, cap_h);
        c->fr->caption_cut = block(c, R_CAPTION, g->line, c->st->caption, x, y, w, wink, &cap, 1) > cap_h + 1.0 / 64;
        y += cap_h + gap;
    }
    /* the keys strip: a keycap a key held, "+" between, centred in the strip's height */
    {
        R strip = rect(x, y, w, keys_h);
        double kx = x, kg = U(c, g->keys_gap), kh = U(c, g->keycap_h), kp = U(c, g->keycap_pad);
        int first = 1, kpart = c->l->part[P_KEYCAP];
        for (k = 0; k < WIKI_NKEYS; k++) {
            TS t;
            int n;
            double tw;
            if (!(c->st->keys >> k & 1u)) continue;
            if (!first) {
                t = ts(c, R_KEYS, g->line);
                n = pens(c, &t, "+", 1);
                tw = ceil64(c->pen[n]);
                line(c, &t, "+", 0, n, kx, y + half(keys_h - t.L), wink, &strip);
                kx += tw + kg;
            }
            first = 0;
            t = ts(c, R_KEYCAP, g->line);
            n = pens(c, &t, KEYCAP[k], strlen(KEYCAP[k]));
            tw = ceil64(c->pen[n]);
            art(c, kpart, rect(kx, y + half(keys_h - kh), kp + tw + kp, kh), &strip);
            line(c, &t, KEYCAP[k], 0, n, kx + kp, y + half(keys_h - kh) + half(kh - t.L), light(c, kpart), &strip);
            kx += kp + tw + kp + kg;
        }
        y += keys_h + gap;
    }
    /* the controls, centred in the row's height (all are ctl_h tall) */
    {
        const int play = c->st->playing && !c->st->done ? c->l->icon_pause : c->l->icon_play;
        double bx = x, cg = U(c, g->ctl_gap);
        bx += button(c, bx, y, "play", "", play, 0, off) + cg;
        bx += button(c, bx, y, "replay", "", c->l->icon_replay, 0, off) + cg;
        bx += button(c, bx, y, "x1", S_X1, -1, c->st->speed == 1, off) + U(c, g->ctl_pair);
        bx += button(c, bx, y, "x2", S_X2, -1, c->st->speed == 2, off) + cg;
        bx += U(c, g->ctl_try);
        button(c, bx, y, "try", c->st->try_it ? S_BACK : S_TRY, -1, c->st->try_it, off);
        y += ctl_h + gap;
    }
    panel(c, WIKI_SCROLL_TEXT, rect(x, y, w, ph), text_rows, "scroll:text");
}

/* ---- the window (#win): its part, title and close, and the body's three columns --------------- */

static void window_(C* c)
{
    const WG* g = &c->l->g;
    const double W = c->st->W, H = c->st->H;
    R win, wt, wx, body;
    double t0, t1, t2, gap;
    win = rect(lu((W - W * g->share / 100) / 2), lu((H - H * g->share / 100) / 2), lu(W * g->share / 100), lu(H * g->share / 100));
    c->fr->window.x = (float)(win.x / c->px); c->fr->window.y = (float)(win.y / c->px);
    c->fr->window.w = (float)(win.w / c->px); c->fr->window.h = (float)(win.h / c->px);
    hot(c, win, NULL, "window");
    art(c, c->l->part[P_WINDOW], win, NULL);
    /* the title in its groove: its own ink, the style's whatever the window's face */
    wt = rect(win.x + U(c, g->wt[0]), win.y + U(c, g->wt[2]), win.w - U(c, g->wt[0]) - U(c, g->wt[1]), U(c, g->wt[3]));
    {
        TS t = ts(c, R_WINDOW_TITLE, g->line);
        int n = pens(c, &t, S_TITLE, strlen(S_TITLE));
        line(c, &t, S_TITLE, 0, n, wt.x + U(c, g->title_pad), wt.y + half(wt.h - t.L), 0, &wt);
    }
    /* the close button in its bay */
    wx = rect(win.x + win.w - U(c, g->wx[0]) - U(c, g->wx[2]), win.y + U(c, g->wx[1]), U(c, g->wx[2]), U(c, g->wx[3]));
    art(c, c->l->part[!strcmp(c->st->held, "close") ? P_CLOSE_DOWN : P_CLOSE], wx, NULL);
    hot(c, wx, NULL, "close");
    glyph(c, c->l->icon_close, wx);
    /* the body: a grid of the two shares and the rest, its gaps between */
    body = rect(win.x + U(c, g->wb[3]), win.y + U(c, g->wb[0]), win.w - U(c, g->wb[3]) - U(c, g->wb[1]),
                win.h - U(c, g->wb[0]) - U(c, g->wb[2]));
    gap = U(c, g->gap);
    t0 = lu(body.w * g->col[0] / 100);
    t1 = lu(body.w * g->col[1] / 100);
    t2 = fmax(0, body.w - t0 - t1 - 2 * gap);
    panel(c, WIKI_SCROLL_TREE, rect(body.x, body.y, t0, body.h), tree_rows, "scroll:tree");
    panel(c, WIKI_SCROLL_DEMOS, rect(body.x + t0 + gap, body.y, t1, body.h), demo_rows, "scroll:demos");
    entry_col(c, rect(body.x + t0 + gap + t1 + gap, body.y, t2, body.h));
}

/* ---- the wiki button (#b-wiki): in the top bar's own pixels, left of the rewind button or in its
   slot, down while the window is open ---------------------------------------------------------- */

static void wiki_button(C* c)
{
    const WG* g = &c->l->g;
    const double k = c->st->bar / 32;
    const double side = U(c, g->btn_side * k);
    R b = rect(c->st->W - U(c, g->btn_right[c->st->rewind ? 0 : 1] * k) - side, U(c, g->btn_top * k), side, side);
    int down = c->st->open || !strcmp(c->st->held, "wiki");
    art(c, c->l->part[down ? P_WIKIBTN_DOWN : P_WIKIBTN], b, NULL);
    hot(c, b, NULL, "wiki");
    glyph(c, c->l->icon_wiki, b);
}

/* ---- the entry points ------------------------------------------------------------------------- */

const char* tagpu_wikilay_mod_name(int mod)
{
    switch (mod) {
    case MOD_RETAIL:   return "stock";
    case MOD_ESC:      return "escalation";
    case MOD_MAYHEM:   return "mayhem";
    case MOD_PROTA:    return "prota";
    case MOD_TAZERO:   return "tazero";
    case MOD_TWILIGHT: return "twilight";
    default:           return "unknown";
    }
}

int tagpu_wikilay_build(const WIKILAY* l, const WIKIDATA* d, const WIKIFACES* f, const WIKIPAGE* st,
                        HUDOPS* out, WIKIFRAME* fr, char* err, size_t errn)
{
    C* c;
    if (err && errn) err[0] = 0;
    if (!l || !d || !f || !st || !out || !fr || !(st->H > 0)) {
        if (err && errn) snprintf(err, errn, "wikilay: no layout, data, faces, page or screen");
        return 0;
    }
    /* the context holds a text's pens and its fills: about 51 KB, off the render thread's stack */
    if (!(c = (C*)malloc(sizeof *c))) {
        if (err && errn) snprintf(err, errn, "wikilay: out of memory");
        return 0;
    }
    memset(c, 0, offsetof(C, cp));
    c->l = l; c->d = d; c->f = f; c->st = st; c->o = out; c->fr = fr;
    c->px = (double)st->H / 720;
    memset(fr, 0, sizeof *fr);
    out->n = out->nrun = out->ntext = out->overflow = out->ngrp = 0;
    out->first = out->last = 0;
    out->track_x0 = out->track_x1 = 0;
    if (st->open) window_(c);
    wiki_button(c);
    free(c);
    return 1;
}

static int find_part(const HUDLAY* k, const char* name);

/* one state of the check built: 0, with the state named in `err`, when the build refuses or drops
   an op, a run, a text, a hit or a group (HUDOPS.overflow) */
static int check_build(const WIKILAY* l, const WIKIDATA* d, const WIKIFACES* f, const WIKIPAGE* st,
                       HUDOPS* scratch, WIKIFRAME* fr, char* err, size_t errn)
{
    if (!tagpu_wikilay_build(l, d, f, st, scratch, fr, err, errn)) return 0;
    if (scratch->overflow) {
        if (err && errn)
            snprintf(err, errn, "wiki layout: the page overflows its ops at %.0fx%.0f (%s, entry %d, demo %d)",
                     st->W, st->H, st->open ? "open" : "closed", st->entry, st->demo);
        return 0;
    }
    return 1;
}

/* THE WORST CASTINGS: every cast bound, its display name 31 em dashes (U+2014, Windows-1252 0x97,
   three bytes of UTF-8 each), the WIKI_TITLE_MAX - 1 bytes of the engine's 31-byte cut and a
   glyph as wide as any, as one word the rules cannot break; and 16 of them with a space between
   each, 31 characters again, which break into the most lines */
static void worst_casting(WIKICASTING* k, int broken)
{
    char name[WIKI_TITLE_MAX];
    size_t n = 0;
    int i, c;
    for (i = 0; i < 31; i++) {
        if (broken && (i & 1)) { name[n++] = ' '; continue; }
        name[n++] = (char)0xE2; name[n++] = (char)0x80; name[n++] = (char)0x94;
    }
    name[n] = 0;
    memset(k, 0, sizeof *k);
    for (c = 0; c < WIKI_NCAST; c++) {
        k->cast[c].bound = 1;
        snprintf(k->cast[c].type, sizeof k->cast[c].type, "WORST");
        memcpy(k->cast[c].title, name, n + 1);
    }
}

/* every state of tagpu_wikilay_check on every screen, filled with casting `k` (NULL: each entry's
   own names), `who` naming it; `whole`: each caption of a demo in turn, and it must show whole,
   else the longest */
static int check_states(const WIKILAY* l, const WIKIDATA* d, const WIKIFACES* f, const WIKICASTING* k, const char* who,
                        int whole, HUDOPS* scratch, char* err, size_t errn)
{
    /* THE SCREENS, in device pixels: 4:3, 16:9 and 32:9. The unit is the height / 720, so the
       window's width in units -- what the text wraps to, and so how many lines and ops a page
       makes -- follows the aspect; these are the common aspects at either end and the one
       between. */
    static const float SCREEN[][2] = { { 1024, 768 }, { 1280, 720 }, { 3840, 1080 } };
    WIKIPAGE st;
    WIKIFRAME fr;
    WIKIWORDS w;
    int e, j, dm, pass, s;
    for (s = 0; s < (int)(sizeof SCREEN / sizeof *SCREEN); s++) {
        memset(&st, 0, sizeof st);
        st.W = SCREEN[s][0];
        st.H = SCREEN[s][1];
        st.bar = 24;
        st.entry = -1;
        st.speed = 1;
        st.iface = 1;
        st.casting = k;
        snprintf(st.held, sizeof st.held, "wiki");
        if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
        st.open = 1;
        snprintf(st.held, sizeof st.held, "close");
        if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
        snprintf(st.held, sizeof st.held, "try");
        for (e = 0; e < d->nentry; e++) {
            const WIKIENTRY* en = &d->entry[e];
            tagpu_wikitext_words(&w, en, k, 1);
            for (dm = 0; dm < (en->ndemo > 0 ? en->ndemo : 1); dm++) {
                const WIKIDEMO* de = dm < en->ndemo ? &en->demo[dm] : NULL;
                int longest = -1;
                size_t most = 0;
                /* the demo's captions, the first state with none: -1 stands for "" */
                for (j = 0; de && j < de->ninput; j++) {
                    char fill[WIKI_CAPTION];
                    if (!(de->input[j].has & WIKI_IN_CAPTION) || !de->input[j].caption) continue;
                    tagpu_wikitext_fill(de->input[j].caption, &w, fill, sizeof fill);
                    if (longest < 0 || strlen(fill) > most) { longest = j; most = strlen(fill); }
                }
                for (j = whole ? -1 : longest; j < (whole && de ? de->ninput : longest + 1); j++) {
                    if (j >= 0 && (!(de->input[j].has & WIKI_IN_CAPTION) || !de->input[j].caption)) continue;
                    st.caption[0] = 0;
                    if (j >= 0) tagpu_wikitext_fill(de->input[j].caption, &w, st.caption, sizeof st.caption);
                    for (pass = 0; pass < 2; pass++) {
                        st.entry = e;
                        st.demo = dm;
                        st.playing = !pass;
                        st.done = pass;
                        st.speed = 1 + pass;
                        if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
                        if (whole && fr.caption_cut) {
                            if (err && errn)
                                snprintf(err, errn, "wiki layout: %s/%s's caption at tick %g passes its strip at %.0fx%.0f "
                                         "with %s", en->id, de->id, de->input[j].at, st.W, st.H, who);
                            return 0;
                        }
                    }
                }
            }
        }
    }
    return 1;
}

int tagpu_wikilay_check(const WIKILAY* l, const WIKIDATA* d, const WIKIFACES* f,
                        const WIKICASTING* castings, int ncasting, HUDOPS* scratch,
                        char* err, size_t errn)
{
    /* the icons the window and the stage draw: the build draws a button without an icon it lacks,
       so their absence is refused here rather than drawn as a blank control */
    static const char* const ICON[] = { "i_wiki", "i_play", "i_pause", "i_replay", "i_open", "i_shut",
                                        "i_dot", "cursor_arrow", "cursor_move", "cursor_build" };
    WIKICASTING worst;
    char who[48];
    int i, b;
    if (err && errn) err[0] = 0;
    if (!l || !d || !f || !scratch || (ncasting > 0 && !castings)) {
        if (err && errn) snprintf(err, errn, "wikilay: no layout, data, faces or scratch to check");
        return 0;
    }
    if (l->icon_close < 0) { if (err && errn) snprintf(err, errn, "wiki kit: no icon 'i_close' or 'i_x'"); return 0; }
    for (i = 0; i < (int)(sizeof ICON / sizeof *ICON); i++)
        if (find_part(l->kit, ICON[i]) < 0) {
            if (err && errn) snprintf(err, errn, "wiki kit: no icon '%s'", ICON[i]);
            return 0;
        }
    if (tagpu_hudlay_style_index(l->kit, "toast") < 0) {
        if (err && errn) snprintf(err, errn, "wiki kit: no `toast` style for the stage");
        return 0;
    }
    if (!check_states(l, d, f, NULL, "the entry's own names", 1, scratch, err, errn)) return 0;
    for (i = 0; i < ncasting; i++) {
        snprintf(who, sizeof who, "casting %d", i);
        if (!check_states(l, d, f, &castings[i], who, 1, scratch, err, errn)) return 0;
    }
    for (b = 0; b < 2; b++) {
        worst_casting(&worst, b);
        if (!check_states(l, d, f, &worst, b ? "the worst names broken" : "the worst names", 0, scratch, err, errn))
            return 0;
    }
    return 1;
}

/* ---- loading ----------------------------------------------------------------------------------- */

typedef struct { const TJV* L; int bad; char* err; size_t errn; } LD;

static double rd(LD* x, const char* a, const char* b, int i)
{
    const TJV* v = tj_get(x->L, a);
    if (b) v = tj_get(v, b);
    if (i >= 0) v = tj_at(v, i);
    if (!tj_is(v, TJ_NUM)) {
        if (!x->bad++ && x->errn) snprintf(x->err, x->errn, "wiki layout: %s%s%s%s is missing or not a number", a, b ? "." : "", b ? b : "", i >= 0 ? "[i]" : "");
        return 0;
    }
    return v->num;
}

static int read_layout(WIKILAY* l, const TJV* L, char* err, size_t errn)
{
    LD x;
    WG* g = &l->g;
    const TJV* kit = tj_get(tj_get(L, "geom"), "kit");
    int i;
    x.L = L; x.bad = 0; x.err = err; x.errn = errn;
    g->btn_side = rd(&x, "button", "side", -1);
    g->btn_top = rd(&x, "button", "top", -1);
    for (i = 0; i < 2; i++) g->btn_right[i] = rd(&x, "button", "right", i);
    g->share = rd(&x, "window", "share", -1);
    g->font = rd(&x, "window", "font", -1);
    g->line = rd(&x, "window", "line", -1);
    {
        LD k = x;
        k.L = kit;
        if (!tj_is(kit, TJ_OBJ) && !x.bad++) snprintf(err, errn, "wiki layout: geom.kit is missing");
        for (i = 0; i < 4; i++) {
            g->wt[i] = rd(&k, "wt", NULL, i); g->wx[i] = rd(&k, "wx", NULL, i); g->wb[i] = rd(&k, "wb", NULL, i);
            g->pn[i] = rd(&k, "pn", NULL, i); g->bz[i] = rd(&k, "bz", NULL, i);
        }
        g->gap = rd(&k, "gap", NULL, -1);
        x.bad += k.bad;
    }
    g->title_pad = rd(&x, "title", "pad", -1);
    for (i = 0; i < 2; i++) g->col[i] = rd(&x, "columns", NULL, i);
    for (i = 0; i < 2; i++) g->panel_pad[i] = rd(&x, "panel", "pad", i);
    g->scrollbar = rd(&x, "scrollbar", NULL, -1);
    g->indent = rd(&x, "tree", "indent", -1);
    g->mark = rd(&x, "tree", "mark", -1);
    g->row_h = rd(&x, "row", "h", -1);
    for (i = 0; i < 2; i++) g->row_pad[i] = rd(&x, "row", "pad", i);
    g->row_gap = rd(&x, "row", "gap", -1);
    g->row_after = rd(&x, "row", "after", -1);
    g->lock = rd(&x, "lock", NULL, -1);
    g->demo_num = rd(&x, "demo", "num", -1);
    for (i = 0; i < 2; i++) g->entry_pad[i] = rd(&x, "entry", "pad", i);
    g->entry_gap = rd(&x, "entry", "gap", -1);
    g->title_line = rd(&x, "entry", "title_line", -1);
    g->badge_h = rd(&x, "badge", "h", -1);
    g->badge_pad = rd(&x, "badge", "pad", -1);
    g->badge_gap = rd(&x, "badge", "gap", -1);
    g->badge_dot = rd(&x, "badge", "dot", -1);
    g->stage_min = rd(&x, "stage", "min", -1);
    g->caption_h = rd(&x, "strips", "caption", -1);
    g->keys_h = rd(&x, "strips", "keys", -1);
    g->keys_gap = rd(&x, "keys", "gap", -1);
    g->keycap_h = rd(&x, "keycap", "h", -1);
    g->keycap_pad = rd(&x, "keycap", "pad", -1);
    g->ctl_gap = rd(&x, "controls", "gap", -1);
    g->ctl_pair = rd(&x, "controls", "pair", -1);
    g->ctl_try = rd(&x, "controls", "try", -1);
    g->ctl_h = rd(&x, "controls", "h", -1);
    g->ctl_pad = rd(&x, "controls", "pad", -1);
    g->ctl_min = rd(&x, "controls", "min", -1);
    g->text_share = rd(&x, "text", "share", -1);
    g->text_para = rd(&x, "text", "para", -1);
    return !x.bad;
}

static int find_part(const HUDLAY* k, const char* name)
{
    int i;
    for (i = 0; i < tagpu_hudlay_nparts(k); i++)
        if (!strcmp(tagpu_hudlay_part(k, i)->name, name)) return i;
    return -1;
}

WIKILAY* tagpu_wikilay_load(const char* layout, size_t nlayout, const char* kit, size_t nkit,
                            HUDPNGSIZE png_size, void* ctx, const HUDLAY* hud, char* err, size_t errn)
{
    WIKILAY* l = (WIKILAY*)calloc(1, sizeof(WIKILAY));
    TJDOC* doc = NULL;
    const TJV* roles;
    const HUDGLOBALS* styles = hud ? tagpu_hudlay_globals(hud) : NULL;
    char e2[200];
    int i, j, ok = 0;
    if (err && errn) err[0] = 0;
    if (!l || !styles) { snprintf(err, errn, "wikilay: out of memory or no styles"); goto done; }
    if (!(doc = tagpu_json_parse(layout, nlayout, e2, sizeof e2))) { snprintf(err, errn, "wiki layout.json: %s", e2); goto done; }
    if (!read_layout(l, tagpu_json_root(doc), err, errn)) goto done;
    if (!(l->kit = tagpu_hudlay_load_kit(hud, kit, nkit, png_size, ctx, e2, sizeof e2))) { snprintf(err, errn, "wiki %s", e2); goto done; }
    for (i = 0; i < P_N; i++)
        if ((l->part[i] = find_part(l->kit, PART[i])) < 0) { snprintf(err, errn, "wiki kit: no part '%s'", PART[i]); goto done; }
    l->icon_close = find_part(l->kit, "i_close");
    if (l->icon_close < 0) l->icon_close = find_part(l->kit, "i_x");
    l->icon_wiki = find_part(l->kit, "i_wiki");
    l->icon_play = find_part(l->kit, "i_play");
    l->icon_pause = find_part(l->kit, "i_pause");
    l->icon_replay = find_part(l->kit, "i_replay");
    l->icon_open = find_part(l->kit, "i_open");
    l->icon_shut = find_part(l->kit, "i_shut");
    l->icon_dot = find_part(l->kit, "i_dot");
    l->nstyle = styles->nstyle;
    memcpy(l->style, styles->style, sizeof l->style);
    roles = tj_get(tagpu_json_root(doc), "roles");
    for (i = 0; i < R_N; i++) {
        const char* sn = tj_str(tj_get(roles, ROLE[i]), NULL);
        for (j = 0; sn && j < l->nstyle && strcmp(l->style[j].name, sn); j++) {}
        if (!sn || j >= l->nstyle) { snprintf(err, errn, "wiki layout: roles.%s names no HUD style", ROLE[i]); goto done; }
        l->role[i] = j;
    }
    ok = 1;
done:
    tagpu_json_free(doc);
    if (!ok) { tagpu_wikilay_free(l); return NULL; }
    return l;
}

void tagpu_wikilay_free(WIKILAY* l)
{
    if (!l) return;
    tagpu_hudlay_free(l->kit);
    free(l);
}

const HUDLAY* tagpu_wikilay_kit(const WIKILAY* l) { return l->kit; }

void tagpu_wikilay_light(WIKILAY* l, int part, int lt)
{
    if (l && part >= 0 && part < MAXPART) l->light[part] = (unsigned char)(lt != 0);
}

int tagpu_wikilay_face_light(const unsigned char* rgba, int w, int h, const float slice[4], int premultiplied)
{
    double t = slice ? slice[0] : h / 4.0, r = slice ? slice[1] : w / 4.0, b = slice ? slice[2] : h / 4.0, le = slice ? slice[3] : w / 4.0;
    double mw = fmax(1, w - le - r), mh = fmax(1, h - t - b), s = 0;
    int i, j, n = 0;
    if (!rgba || w < 1 || h < 1) return 0;
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++) {
            int x0 = (int)floor(le + mw * i / 8), x1 = (int)ceil(le + mw * (i + 1) / 8);
            int y0 = (int)floor(t + mh * j / 8), y1 = (int)ceil(t + mh * (j + 1) / 8), x, y, k = 0;
            double cr = 0, cg = 0, cb = 0, ca = 0;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            /* the canvas averages premultiplied colour and reads the cell back straight */
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    const unsigned char* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    const double a = p[3], f = premultiplied ? 1 : a / 255;
                    cr += p[0] * f; cg += p[1] * f; cb += p[2] * f; ca += a;
                    k++;
                }
            if (!k || ca / k <= 128) continue;
            s += (0.3 * cr + 0.59 * cg + 0.11 * cb) / ca * 255;
            n++;
        }
    return n > 0 && s / n > 120;
}
