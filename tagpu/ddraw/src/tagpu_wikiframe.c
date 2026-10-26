/* tagpu_wikiframe.c -- the in-game wiki's shell laid out. Contract: tagpu_wikiframe.h.

   Each function is one of the mockup's rules, in its order: the window over the game, then the
   wiki button over it (wiki.css gives the button z-index 7, above the window's 6, because it
   closes the window too). */
#include <stdio.h>
#include <string.h>
#include "tagpu_wikiframe.h"

/* THE WINDOW'S SHARE of the screen, each way, centred: the mockup's default `size=90` */
#define SHARE 0.90f

/* WHAT THE KIT'S WINDOW ART LEAVES ROOM FOR, in units: wiki.js GEOM.kit, MEASURED there 2026-10-25
   from the kit's window.png at its scale. The title in its dark groove (left, right, top, height),
   its text 6 units in (wiki.css `#win .wt` padding-left); the close button centred in its bay
   (right, top, width, height); the body inside the frame (top, right, bottom, left). */
static const float WT[4] = { 2.6f, 20.4f, 2.4f, 15.7f };
#define WT_PAD 6.0f
static const float WX[4] = { 3.5f, 3.2f, 14.0f, 14.0f };
static const float WB[4] = { 23.0f, 5.6f, 5.6f, 5.5f };

/* THE TOP BAR'S BUTTONS, in the bar's own pixels (wiki.css .tb, tagpu_menu.c trigger_rect): 28
   square at y 2; the gear's right edge 16 from the screen's, the rewind button's 47, the wiki
   button's 78 -- 3 left of the rewind button -- or 47, in the rewind button's slot, when this
   frame draws no rewind button */
#define TB_SIDE    28.0f
#define TB_TOP     2.0f
#define TB_R_NEXT  78.0f
#define TB_R_SLOT  47.0f

/* a glyph on a button: as large as fits its box, then 0.6 of that, centred (wiki.js drawIcon) */
#define ICON_F     0.6f

#define TITLE "Wiki"

typedef struct {
    const HUDLAY* kit;
    HUDOPS*       o;
    char*         err;
    size_t        errn;
    int           bad;
} C;

static HUDRECT box(float x, float y, float w, float h)
{
    HUDRECT r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static HUDOP* op(C* c, int kind, HUDRECT r)
{
    HUDOP* p;
    if (c->o->n >= HUD_MAXOP) { c->o->overflow = 1; return NULL; }
    p = &c->o->op[c->o->n++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->r = r;
    p->clip.w = -1;
    p->part = -1;
    p->style = -1;
    p->rgba = 0xFFFFFFFFu;
    return p;
}

static int text_put(C* c, const char* s)
{
    size_t n = strlen(s);
    int at = c->o->ntext;
    if ((size_t)at + n + 1 > HUD_TEXTMAX) { c->o->overflow = 1; return -1; }
    memcpy(c->o->text + at, s, n + 1);
    c->o->ntext += (int)n + 1;
    return at;
}

static void fail(C* c, const char* what, const char* name)
{
    if (!c->bad++ && c->err && c->errn) snprintf(c->err, c->errn, "wiki: no %s '%s' in the kit", what, name);
}

static void art(C* c, const char* part, HUDRECT r)
{
    int k = tagpu_hudlay_part_index(c->kit, part);
    HUDOP* p;
    if (k < 0) { fail(c, "part", part); return; }
    if (!(p = op(c, HOP_PART, r))) return;
    p->part = (int16_t)k;
}

static void icon(C* c, const char* part, HUDRECT r)
{
    float w = r.w * ICON_F, h = r.h * ICON_F;
    art(c, part, box(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h));
}

static void hot(C* c, HUDRECT r, const char* key)
{
    HUDOP* p = op(c, HOP_HOT, r);
    int at;
    if (!p) return;
    if ((at = text_put(c, key)) < 0) { c->o->n--; return; }
    p->run = (uint16_t)at;
}

/* one line of text in style `style` from the box's left, its line centred in the box's height
   (tagpu_hudlay_baseline, as the groove's `align-items: center` places it), clipped to the box */
static void text(C* c, const char* s, HUDRECT r, const char* style)
{
    int st = tagpu_hudlay_style_index(c->kit, style), off;
    HUDOP* p;
    HUDRUN* run;
    if (st < 0) { fail(c, "style", style); return; }
    if (c->o->nrun + 1 > HUD_MAXRUN) { c->o->overflow = 1; return; }
    if (!(p = op(c, HOP_TEXT, r))) return;
    if ((off = text_put(c, s)) < 0) { c->o->n--; return; }
    p->style = (int16_t)st;
    p->centre = 0;
    p->size = tagpu_hudlay_globals(c->kit)->style[st].size;
    p->clip = r;
    p->run = (uint16_t)c->o->nrun;
    p->nrun = 1;
    run = &c->o->run[c->o->nrun++];
    run->rgb = 0;
    run->resource = 0;
    run->off = (uint16_t)off;
    run->len = (uint16_t)strlen(s);
}

/* the window: its frame, the title in the groove, the close button in its bay, the body. The whole
   window is a control placed before the close button, so the hit test, topmost first, finds the
   close button over it */
static void window_(C* c, const WIKIFRAME* in, HUDRECT* body)
{
    float w = in->W * SHARE, h = in->H * SHARE;
    HUDRECT win = box((in->W - w) / 2, (in->H - h) / 2, w, h), x;
    hot(c, win, "window");
    art(c, "window", win);
    text(c, TITLE, box(win.x + WT[0] + WT_PAD, win.y + WT[2], win.w - WT[0] - WT[1] - WT_PAD, WT[3]), "title");
    x = box(win.x + win.w - WX[0] - WX[2], win.y + WX[1], WX[2], WX[3]);
    art(c, in->x_down ? "close_down" : "close", x);
    icon(c, "i_close", x);
    hot(c, x, "close");
    *body = box(win.x + WB[3], win.y + WB[0], win.w - WB[1] - WB[3], win.h - WB[0] - WB[2]);
    /* THE PAGE IS THE NEXT LANDING'S: until then the body is one empty panel */
    art(c, "panel", *body);
}

/* the wiki button, in the bar's own pixels as tagpu_hudlay.c `live` places the rewind button;
   pressed while the window is open, as the mockup's `.on` */
static void button(C* c, const WIKIFRAME* in)
{
    float k = in->bar, s = TB_SIDE * k;
    HUDRECT b = box(in->W - ((in->rewind ? TB_R_NEXT : TB_R_SLOT) + TB_SIDE) * k, TB_TOP * k, s, s);
    art(c, in->open || in->btn_down ? "wikibtn_down" : "wikibtn", b);
    icon(c, "i_wiki", b);
    hot(c, b, "wiki");
}

int tagpu_wikiframe_build(const HUDLAY* kit, const WIKIFRAME* in, HUDOPS* out, HUDRECT* body,
                          char* err, size_t errn)
{
    C c;
    memset(&c, 0, sizeof c);
    c.kit = kit; c.o = out; c.err = err; c.errn = errn;
    out->n = out->nrun = out->ntext = out->overflow = out->ngrp = 0;
    out->first = out->last = 0;
    out->track_x0 = out->track_x1 = 0;
    *body = box(0, 0, 0, 0);
    if (err && errn) err[0] = 0;
    if (in->open) window_(&c, in, body);
    button(&c, in);
    return !c.bad;
}
