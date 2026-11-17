/* tagpu_wikilay.c -- the in-game wiki's window. Contract: tagpu_wikilay.h.

   EACH FUNCTION BELOW IS ONE OF THE MOCKUP'S RULES, IN ITS ORDER: a box of wiki.css with the
   content wiki.js renders into it, emitted in the document's order, which is the order the
   browser paints them. A rule changed in one place is changed in the other; wikilaycheck.py is
   what notices when they disagree.

   Its widgets, and the rounding and line rules a rule's arithmetic rests on: tagpu_uikit.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_wikilay.h"
#include "tagpu_wikitext.h"
#include "tagpu_json.h"
#include "tagpu_mod.h"

/* the kit's parts the rules draw, by name; the icons are looked up as the mockup's data-glyph
   and data-part list them, the first the kit has. The control glyphs and the tree's marks are
   icons because no HUD face has their characters (tools/wiki/mockup/glyphs.js draws them) */
enum { P_WINDOW, P_CLOSE, P_CLOSE_DOWN, P_PANEL, P_ROW, P_ROW_SEL, P_BEZEL, P_BUTTON, P_BUTTON_DOWN,
       P_BADGE, P_KEYCAP, P_WIKI, P_WIKI_DOWN, P_CONFIG, P_CONFIG_DOWN, P_N };
static const char* const PART[P_N] = { "window", "close", "close_down", "panel", "row", "row_selected",
    "bezel", "button", "button_down", "badge", "keycap", "wiki_emblem", "wiki_emblem_down", "config_emblem",
    "config_emblem_down" };

/* layout.json `roles`, the text of each element */
enum { R_WINDOW_TITLE, R_ENTRY_TITLE, R_CATEGORY, R_ENTRY, R_DEMO, R_DEMO_NUM, R_SUBTITLE, R_TEXT,
       R_CHIP, R_CAPTION, R_CONTROL, R_BADGE, R_KEYCAP, R_KEYS, R_N };
static const char* const ROLE[R_N] = { "window_title", "entry_title", "category", "entry", "demo",
    "demo_num", "subtitle", "text", "chip", "caption", "control", "badge", "keycap", "keys" };

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
#define S_ABOUT     "About"
#define S_FIT       "Fit"
#define S_ZOUT      "\xE2\x88\x92"         /* − (U+2212, &minus;) */
#define S_ZIN       "+"
#define S_LINKS     "All links"
/* a keycap a WIKI_KEY_*, in their order: one for every key, which the assert holds, so no key held
   shows another's name or none */
static const char* const KEYCAP[] = { "Shift", "Alt", "Ctrl", "Z", "X", "E", "Left-click", "Right-click" };
typedef char wikilay_keycaps_fit[sizeof KEYCAP / sizeof *KEYCAP == WIKI_NKEYS ? 1 : -1];

#define MAXFILL     (4 * UI_MAXGLYPH + 1)   /* a filled text's bytes: the code points the rules set,
                                               four bytes at most each, and the terminator; a
                                               longer fill is cut at a character (tagpu_wikitext.h) */
enum { FILL_TITLE, FILL_SUB, FILL_ROW, FILL_N };    /* the texts filled at once: the entry's title
                                               and subtitle are measured and then set, and a row,
                                               a paragraph or a demo title is set as it is filled */

/* layout.json's numbers, in units unless named */
typedef struct {
    double btn_side, btn_top, btn_right[2], btn_config;
    double share, font, line;               /* the window's share (percent), its font, its line height */
    double wt[4], wx[4], wb[4], gap, bz[4]; /* geom.kit: wt l r t h, wx r t w h, the quads t r b l */
    UIPANELG pnl;                           /* its pn is geom.kit's                               */
    UIROWG   row;
    UIBUTTONG ctl;
    UIBADGEG  bdg;
    double title_pad;
    double col[2];                          /* percent */
    double indent, mark;
    double demo_num;
    double entry_pad[2], entry_gap, title_line;
    double stage_min;
    double caption_h, keys_h, keys_gap;
    double keycap_h, keycap_pad;
    double ctl_gap, ctl_pair, ctl_try;
    double text_share, text_para;
    double theatre_about, theatre_min;      /* the About panel's share of the body (percent), the
                                               theatre stage's least height                       */
    double theatre_about_max;               /* the About panel's most width, in units             */
} WG;

struct WIKILAY {
    HUDLAY*          kit;
    WG               g;
    int              part[P_N];
    int              icon_close;                /* -1: the kit has none */
    int              icon_play, icon_pause, icon_replay, icon_open, icon_shut, icon_dot;
    int              role[R_N];                 /* a style index */
    int              nstyle;
    struct HUDSTYLE  style[24];
    unsigned char    light[UI_MAXPART];
};

typedef struct {
    UI               u;
    const WIKILAY*   l;
    const WIKIDATA*  d;
    const WIKIPAGE*  st;
    WIKIFRAME*       fr;
    char             fill[FILL_N][MAXFILL];     /* the texts filled, by FILL_*                   */
    UIR              top_box;                   /* the theatre's top layer: its box, and          */
    int              top_kind;                  /* TOP_* (built after the window: see theatre)    */
} C;

enum { TOP_NONE, TOP_ABOUT, TOP_UNIT };

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

/* a layout number in units, as a CSS length */
static double U(const C* c, double n) { return ui_len(&c->u, n); }
/* a role's text style, and its block */
static UITS ts(const C* c, int role, double line) { return ui_ts(&c->u, c->l->role[role], line); }
static double block(C* c, int role, double lh, const char* s, double x, double y, double w, int ink, const UIR* clip, int emit)
{
    return ui_block(&c->u, c->l->role[role], lh, s, x, y, w, ink, clip, emit);
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

/* A theatre's entry heads its own demos: their rows under its row, `w` wide at `x`, from `y0` in the
   list, the selected row being the demo. Returns their height. */
static double tree_demos(C* c, const UIPANEL* p, const WIKIENTRY* e, double x, double y0, double w, int emit)
{
    double y = 0;
    int i;
    char key[HUD_KEYLEN];
    for (i = 0; i < e->ndemo; i++) {
        UIROW rw;
        memset(&rw, 0, sizeof rw);
        rw.part = c->l->part[i == c->st->demo ? P_ROW_SEL : P_ROW];
        rw.style = c->l->role[R_DEMO];
        rw.text = filled_or(c, FILL_ROW, e, e->demo[i].title, e->demo[i].id);
        rw.mark = "";
        rw.mark_icon = -1;
        snprintf(key, sizeof key, "demo:%d", i);
        rw.key = key;
        y += ui_row(&c->u, &c->l->g.row, &rw, x, p->cy + y0 + y, w, &p->view, emit);
    }
    return y;
}

/* the tree's rows `w` wide, in preorder, a folded category's descendants left out: returns their
   height */
static double tree_rows(UI* u, const UIPANEL* p, double w, int emit, void* user)
{
    C* c = (C*)user;
    const WG* g = &c->l->g;
    double y = 0;
    int i, shut = -1;
    char key[HUD_KEYLEN];
    for (i = 0; i < c->d->nnode && i < WIKI_MAXNODE; i++) {
        const WIKINODE* nd = &c->d->node[i];
        UIROW rw;
        double ind = nd->depth * U(c, g->indent);
        if (shut >= 0 && nd->depth > shut) continue;
        shut = -1;
        memset(&rw, 0, sizeof rw);
        rw.part = c->l->part[P_ROW];
        rw.style = c->l->role[nd->entry >= 0 ? R_ENTRY : R_CATEGORY];
        rw.mark = "";
        rw.mark_w = U(c, g->mark);
        if (nd->entry >= 0) {
            const WIKIENTRY* e = &c->d->entry[nd->entry];
            const int game = e->frame == WIKI_FRAME_GAME, sel = nd->entry == c->st->entry;
            rw.text = filled_or(c, FILL_ROW, e, e->title, e->id);
            rw.mark_icon = !game ? c->l->icon_dot : sel ? c->l->icon_open : c->l->icon_shut;
            rw.locked = locked(c, nd->entry);
            if (sel && !game) rw.part = c->l->part[P_ROW_SEL];
            snprintf(key, sizeof key, "entry:%d", i);
            rw.key = key;
            y += ui_row(&c->u, &c->l->g.row, &rw, p->cx + ind, p->cy + y, w - ind, &p->view, emit);
            if (sel && game) {
                const double in2 = ind + U(c, g->indent);
                y += tree_demos(c, p, e, p->cx + in2, y, w - in2, emit);
            }
            continue;
        } else {
            rw.text = nd->title ? nd->title : "";
            rw.mark_icon = folded(c->st, i) ? c->l->icon_shut : c->l->icon_open;
            if (folded(c->st, i)) shut = nd->depth;
            snprintf(key, sizeof key, "fold:%d", i);
        }
        rw.key = key;
        y += ui_row(&c->u, &c->l->g.row, &rw, p->cx + ind, p->cy + y, w - ind, &p->view, emit);
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

static double demo_rows(UI* u, const UIPANEL* p, double w, int emit, void* user)
{
    C* c = (C*)user;
    const WG* g = &c->l->g;
    const WIKIENTRY* e = entry(c);
    double y = 0;
    int i;
    char key[HUD_KEYLEN], num[16];
    UIROW rw;
    if (!e || !e->ndemo) {
        /* li.none: "No demos", or nothing with no entry; no part, the panel's ink */
        memset(&rw, 0, sizeof rw);
        rw.part = -1;
        rw.style = c->l->role[R_DEMO];
        rw.text = e ? S_NODEMOS : "";
        rw.mark = "";
        rw.mark_icon = -1;
        rw.ink = ui_light(&c->u, c->l->part[P_PANEL]);
        return ui_row(&c->u, &c->l->g.row, &rw, p->cx, p->cy, w, &p->view, emit);
    }
    for (i = 0; i < e->ndemo; i++) {
        memset(&rw, 0, sizeof rw);
        rw.part = c->l->part[i == c->st->demo ? P_ROW_SEL : P_ROW];
        rw.style = c->l->role[R_DEMO];
        rw.text = filled_or(c, FILL_ROW, e, e->demo[i].title, e->demo[i].id);
        snprintf(num, sizeof num, "%d", i + 1);
        rw.mark = num;
        rw.mark_style = c->l->role[R_DEMO_NUM];
        rw.mark_icon = -1;
        rw.mark_w = U(c, g->demo_num);
        rw.mark_right = 1;
        snprintf(key, sizeof key, "demo:%d", i);
        rw.key = key;
        y += ui_row(&c->u, &c->l->g.row, &rw, p->cx, p->cy + y, w, &p->view, emit);
    }
    return y;
}

/* ---- the entry's text (#text): its paragraphs, each with its gap after it -------------------- */

static double text_rows(UI* u, const UIPANEL* p, double w, int emit, void* user)
{
    C* c = (C*)user;
    const WIKIENTRY* e = entry(c);
    double y = 0;
    int i, ink = ui_light(&c->u, c->l->part[P_PANEL]);
    for (i = 0; e && i < e->ntext; i++) {
        y += block(c, R_TEXT, c->l->g.line, filled(c, FILL_ROW, e, e->text[i]), p->cx, p->cy + y, w, ink, &p->view, emit);
        y += U(c, c->l->g.text_para);
    }
    return y;
}

/* A panel at `box` with its list: its scroll is the page's, and what it answers the frame's */
static void panel(C* c, int which, UIR box, UILIST list, const char* key)
{
    UISCROLL a;
    ui_panel(&c->u, &c->l->g.pnl, c->l->part[P_PANEL], box, c->st->scroll[which], list, c, key, &a);
    c->fr->scroll[which] = a.scroll;
    c->fr->scroll_max[which] = a.scroll_max;
    c->fr->track[which] = a.track;
    c->fr->thumb_y[which] = a.thumb_y;
    c->fr->thumb_h[which] = a.thumb_h;
}

/* ---- the controls (#ctl): play, replay, 1x and 2x, Try it yourself ----------------------------- */

/* A button: its part, down while on or held. */
static double button(C* c, double x, double y, const char* key, const char* label, int icon, int on, int off)
{
    const int down = on || !strcmp(c->st->held, key);
    return ui_button(&c->u, &c->l->g.ctl, x, y, key, label, icon, c->l->part[down ? P_BUTTON_DOWN : P_BUTTON], off);
}

/* The lock badge at (x, y) (#badge, #t-badge), never stretched: returns its width. */
static double badge(C* c, double x, double y) { return ui_badge(&c->u, &c->l->g.bdg, c->l->part[P_BADGE], x, y, S_BADGE); }

/* The stage's bezel `sbox` (#sbox) and the stage inside it (#stage), inset by the bezel's frame:
   its hot, and its rect answered in the frame */
static void stage_in(C* c, UIR sbox)
{
    const WG* g = &c->l->g;
    UIR stage = ui_rect(sbox.x + U(c, g->bz[3]), sbox.y + U(c, g->bz[0]), ui_lu(sbox.w - (g->bz[3] + g->bz[1]) * c->u.px),
                   ui_lu(sbox.h - (g->bz[0] + g->bz[2]) * c->u.px));
    ui_art(&c->u, c->l->part[P_BEZEL], sbox, NULL);
    ui_hot(&c->u, stage, NULL, "stage");
    c->fr->stage.x = (float)(stage.x / c->u.px); c->fr->stage.y = (float)(stage.y / c->u.px);
    c->fr->stage.w = (float)(stage.w / c->u.px); c->fr->stage.h = (float)(stage.h / c->u.px);
}

/* The caption (#caption) `w` wide at (x, y): a fixed height, so a caption appearing cannot move the
   stage; overflow hidden, so a line past the strip is cut (to a sixty-fourth of a pixel, Chrome's
   LayoutUnit) */
static void caption(C* c, double x, double y, double w)
{
    const WG* g = &c->l->g;
    const double cap_h = U(c, g->caption_h);
    UIR cap = ui_rect(x, y, w, cap_h);
    c->fr->caption_cut = block(c, R_CAPTION, g->line, c->st->caption, x, y, w, ui_light(&c->u, c->l->part[P_WINDOW]), &cap, 1)
                         > cap_h + 1.0 / 64;
}

/* The tech tree's controls (#ctl in #win.techtree): Fit, the zoom steps out and in and All links
   (#tt-ctl, the controls' pair between them), then About; none is ever disabled, All links is down
   while on. Returns their width; draws only with `emit`. */
static double tech_controls(C* c, double x, double y, int emit)
{
    const WG* g = &c->l->g;
    const double cg = U(c, g->ctl_gap), pair = U(c, g->ctl_pair);
    const struct { const char* key; const char* label; int on; double after; } b[] = {
        { "fit", S_FIT, 0, pair }, { "zout", S_ZOUT, 0, pair }, { "zin", S_ZIN, 0, pair },
        { "links", S_LINKS, c->st->tech_all != 0, cg }, { "about", S_ABOUT, c->st->about != 0, 0 } };
    double bx = x;
    int i;
    for (i = 0; i < 5; i++) {
        bx += emit ? button(c, bx, y, b[i].key, b[i].label, -1, b[i].on, 0) : ui_button_w(&c->u, &c->l->g.ctl, b[i].label);
        if (i + 1 < 5) bx += b[i].after;
    }
    return bx - x;
}

/* The controls (#ctl) from (x, y), each ctl_h tall: play, replay, 1x and 2x, Try it yourself, and
   in the theatre About (#c-about), which is never disabled. Returns their width; draws only with
   `emit`. */
static double controls(C* c, double x, double y, int about, int emit)
{
    const WG* g = &c->l->g;
    const int play = c->st->playing && !c->st->done ? c->l->icon_pause : c->l->icon_play, off = !has_demo(c);
    const double cg = U(c, g->ctl_gap), pair = U(c, g->ctl_pair);
    /* each with the space after it: the gap, the pair's between 1x and 2x, and Try it's margin */
    const struct { const char* key; const char* label; int icon, on, off; double after; } b[] = {
        { "play", "", play, 0, off, cg },
        { "replay", "", c->l->icon_replay, 0, off, cg },
        { "x1", S_X1, -1, c->st->speed == 1, off, pair },
        { "x2", S_X2, -1, c->st->speed == 2, off, cg + U(c, g->ctl_try) },
        { "try", c->st->try_it ? S_BACK : S_TRY, -1, c->st->try_it, off, cg },
        { "about", S_ABOUT, -1, c->st->about != 0, 0, 0 } };
    const int nb = about ? 6 : 5;
    double bx = x;
    int i;
    for (i = 0; i < nb; i++) {
        bx += emit ? button(c, bx, y, b[i].key, b[i].label, b[i].icon, b[i].on, b[i].off) : ui_button_w(&c->u, &c->l->g.ctl, b[i].label);
        if (i + 1 < nb) bx += b[i].after;
    }
    return bx - x;
}

/* ---- the entry (#entry): a flex column of title, subtitle, badge, the stage row, caption, keys,
   controls and the text, the stage row taking what the others leave ---------------------------- */

static void entry_col(C* c, UIR col)
{
    const WG* g = &c->l->g;
    const WIKIENTRY* e = entry(c);
    const double gap = U(c, g->entry_gap), x = col.x + U(c, g->entry_pad[1]), w = col.w - 2 * U(c, g->entry_pad[1]);
    const double top = col.y + U(c, g->entry_pad[0]), ch = col.h - U(c, g->entry_pad[0]);
    const int wink = ui_light(&c->u, c->l->part[P_WINDOW]);
    const int lk = locked(c, c->st->entry) || c->st->sim_locked;
    /* a text entry (tagpu_wikidata_entry): a page entry with no demos */
    const int text = e && !e->ndemo && e->frame == WIKI_FRAME_PAGE;
    const char* title = e ? filled_or(c, FILL_TITLE, e, e->title, e->id) : S_CHOOSE;
    const char* sub = e ? filled(c, FILL_SUB, e, e->subtitle) : "";
    double th, sh, bh, ph, fixed, srow, y, ctl_h = U(c, g->ctl.h), cap_h = U(c, g->caption_h), keys_h = U(c, g->keys_h);
    int items, k;

    th = block(c, R_ENTRY_TITLE, g->title_line, title, x, 0, w, 0, NULL, 0);
    sh = block(c, R_SUBTITLE, g->line, sub, x, 0, w, 0, NULL, 0);
    bh = lk ? U(c, g->bdg.h) : 0;
    ph = ui_lu(ch * g->text_share / 100);
    items = 7 + lk;
    fixed = th + sh + bh + cap_h + keys_h + ctl_h + ph + gap * (items - 1);
    srow = fmax(U(c, g->stage_min), ch - fixed);

    y = top;
    block(c, R_ENTRY_TITLE, g->title_line, title, x, y, w, wink, NULL, 1);
    y += th + gap;
    block(c, R_SUBTITLE, g->line, sub, x, y, w, wink, NULL, 1);
    y += sh + gap;
    if (lk) {
        /* #badge: align-self flex-start, so as wide as its content */
        badge(c, x, y);
        y += bh + gap;
    }
    if (text) {
        /* #entry.text: the column is the title, the subtitle, the badge and the text, which takes
           the column's rest as the stage row takes it on a demo's page (flex 1 1 0). No stage,
           caption, keys strip or controls, so no `stage` hit is drawn and the frame record's
           stage rect stays zero: nothing on the render thread looks up a stage for it. */
        panel(c, WIKI_SCROLL_TEXT, ui_rect(x, y, w, fmax(0, ch - (th + sh + bh + gap * (2 + lk)))), text_rows,
              "scroll:text");
        return;
    }
    /* the stage row: the bezel, the row's full width */
    stage_in(c, ui_rect(x, y, w, srow));
    y += srow + gap;
    caption(c, x, y, w);
    y += cap_h + gap;
    /* the keys strip: a keycap a key held, "+" between, centred in the strip's height */
    {
        UIR strip = ui_rect(x, y, w, keys_h);
        double kx = x, kg = U(c, g->keys_gap), kh = U(c, g->keycap_h), kp = U(c, g->keycap_pad);
        int first = 1, kpart = c->l->part[P_KEYCAP];
        for (k = 0; k < WIKI_NKEYS; k++) {
            UITS t;
            int n;
            double tw;
            if (!(c->st->keys >> k & 1u)) continue;
            if (!first) {
                t = ts(c, R_KEYS, g->line);
                n = ui_pens(&c->u, &t, "+", 1);
                tw = ui_ceil64(c->u.pen[n]);
                ui_line(&c->u, &t, "+", 0, n, kx, y + ui_half(keys_h - t.L), wink, &strip);
                kx += tw + kg;
            }
            first = 0;
            t = ts(c, R_KEYCAP, g->line);
            n = ui_pens(&c->u, &t, KEYCAP[k], strlen(KEYCAP[k]));
            tw = ui_ceil64(c->u.pen[n]);
            ui_art(&c->u, kpart, ui_rect(kx, y + ui_half(keys_h - kh), kp + tw + kp, kh), &strip);
            ui_line(&c->u, &t, KEYCAP[k], 0, n, kx + kp, y + ui_half(keys_h - kh) + ui_half(kh - t.L), ui_light(&c->u, kpart), &strip);
            kx += kp + tw + kp + kg;
        }
        y += keys_h + gap;
    }
    controls(c, x, y, 0, 1);
    y += ctl_h + gap;
    panel(c, WIKI_SCROLL_TEXT, ui_rect(x, y, w, ph), text_rows, "scroll:text");
}

/* ---- the theatre (#win.theatre): the page of an entry with "frame": "game", whose stage holds a
   whole game frame (research/notes/ingame-wiki-replay-plan.md, decisions 6 and 24) ------------- */

/* About (#about): the panel's part over `box`, and inside it, padded by the panel's frame and the
   entry's padding together, a flex column of the entry's title, its subtitle, the lock badge and
   the scrolling text, which takes what the others leave. Its text is in the panel's ink. */
static void about_panel(C* c, UIR box)
{
    const WG* g = &c->l->g;
    const WIKIENTRY* e = entry(c);
    const double pt = U(c, g->pnl.pn[0] + g->entry_pad[0]), pr = U(c, g->pnl.pn[1] + g->entry_pad[1]);
    const double pb = U(c, g->pnl.pn[2] + g->entry_pad[0]), pl = U(c, g->pnl.pn[3] + g->entry_pad[1]);
    const double gap = U(c, g->entry_gap), x = box.x + pl, w = box.w - pl - pr, ch = box.h - pt - pb;
    const int ink = ui_light(&c->u, c->l->part[P_PANEL]), lk = locked(c, c->st->entry) || c->st->sim_locked;
    const char* title = filled_or(c, FILL_TITLE, e, e->title, e->id);
    const char* sub = filled(c, FILL_SUB, e, e->subtitle);
    double th, sh, bh = lk ? U(c, g->bdg.h) : 0, y = box.y + pt;
    ui_art(&c->u, c->l->part[P_PANEL], box, NULL);
    th = block(c, R_ENTRY_TITLE, g->title_line, title, x, y, w, ink, NULL, 1);
    y += th + gap;
    sh = block(c, R_SUBTITLE, g->line, sub, x, y, w, ink, NULL, 1);
    y += sh + gap;
    if (lk) {
        badge(c, x, y);
        y += bh + gap;
    }
    panel(c, WIKI_SCROLL_TEXT, ui_rect(x, y, w, fmax(0, ch - th - sh - bh - gap * (2 + lk))), text_rows, "scroll:text");
}

/* ---- the tech tree's unit panel (#unit, wiki.js techPanel): its picture, name, description and
   flags; the 3D view and its actions (unitview.js); its figures and weapons; the walk to it; who
   builds it; its upgrades; what it builds, by kind. Each chip picks its unit (unit:<order>). ---- */

#define UP_RGB    0xB56AD0u     /* an upgrade's purple (#unit .flag.up, .chip.up, .chip.has) */
#define SEL_RGB   0xE8C66Au     /* the picked chip's outline (.chip.sel)                     */
#define VIEW_RGB  0x12140Fu     /* the view's ground, under the 3D view                      */
#define S_ELLIPSIS "..."       /* three periods: the HUD faces have no ellipsis character */
#define S_SEP     "\xC2\xBB"   /* » (U+00BB), which the faces have */
#define UNIT_MAXLIST 1024       /* a chip list's entries: a menu's, at most a side's walk     */

/* the unit of order index `k`'s text, UTF-8: its title (the name when it has none), its
   description */
static void unit_title(const WIKITECHTREE* T, int k, char* d, size_t n)
{
    const WIKITECHUNIT* x = &T->t->unit[T->unit[k]];
    if (x->title[0]) tagpu_wikitech_utf8(d, n, x->title, 1);
    else snprintf(d, n, "%s", x->name);
}

/* the panel's ink: dark on a light panel, else the text style's own colour */
static uint32_t unit_ink_rgb(const C* c, int role)
{
    return ui_light(&c->u, c->l->part[P_PANEL]) ? 0x1D1D1Bu : c->l->style[c->l->role[role]].rgb;
}

/* the last text op, if `n0` was the op count before it, recoloured `rgb`: its run's colour, the
   shadow or outline the face gives it kept */
static void recolour(C* c, int n0, uint32_t rgb)
{
    HUDOP* p;
    if (c->u.o->n <= n0) return;
    p = &c->u.o->op[c->u.o->n - 1];
    if (p->kind == HOP_TEXT && p->nrun) c->u.o->run[p->run].rgb = rgb;
}

/* a 1 px frame `t` thick round `r`, inset by `in` (CSS outline with outline-offset -in): solid,
   or dashed in three-pixel dashes */
static void frame_px(C* c, UIR r, double t, double in, uint32_t rgb, int dashed, const UIR* clip)
{
    const UIR o = ui_rect(r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in);
    if (!dashed) {
        ui_solid(&c->u, HOP_SOLID, ui_rect(o.x, o.y, o.w, t), rgb, 1, clip);
        ui_solid(&c->u, HOP_SOLID, ui_rect(o.x, o.y + o.h - t, o.w, t), rgb, 1, clip);
        ui_solid(&c->u, HOP_SOLID, ui_rect(o.x, o.y + t, t, o.h - 2 * t), rgb, 1, clip);
        ui_solid(&c->u, HOP_SOLID, ui_rect(o.x + o.w - t, o.y + t, t, o.h - 2 * t), rgb, 1, clip);
    } else {
        double a;
        for (a = 0; a < o.w; a += 6) {
            ui_solid(&c->u, HOP_SOLID, ui_rect(o.x + a, o.y, fmin(3, o.w - a), t), rgb, 1, clip);
            ui_solid(&c->u, HOP_SOLID, ui_rect(o.x + a, o.y + o.h - t, fmin(3, o.w - a), t), rgb, 1, clip);
        }
        for (a = 0; a < o.h; a += 6) {
            ui_solid(&c->u, HOP_SOLID, ui_rect(o.x, o.y + a, t, fmin(3, o.h - a)), rgb, 1, clip);
            ui_solid(&c->u, HOP_SOLID, ui_rect(o.x + o.w - t, o.y + a, t, fmin(3, o.h - a)), rgb, 1, clip);
        }
    }
}

/* a 128-bit unsigned, for js_fixed's exact compare: the C the DLL builds with has no wider type */
typedef struct { uint64_t hi, lo; } JSU128;

static JSU128 js_mul(uint64_t a, uint64_t b)
{
    const uint64_t al = a & 0xFFFFFFFFu, ah = a >> 32, bl = b & 0xFFFFFFFFu, bh = b >> 32;
    const uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
    JSU128 r;
    r.lo = (mid << 32) | (ll & 0xFFFFFFFFu);
    r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return r;
}

/* |v| * p >= q / 2, exactly: |v| is m * 2^-s (m < 2^53, 0 < s < 1100), so the test is
   m * p * 2 >= q * 2^s, both sides in 128 bits. m * p * 2 < 2^53 * 10^4 * 2 < 2^68; a right side
   past 2^127 is larger than any left one. */
static int js_ge_half(uint64_t m, int s, uint64_t p, uint64_t q)
{
    const JSU128 a = js_mul(m, p * 2);
    JSU128 b;
    if (q == 0) return 1;
    if (s >= 128) return 0;
    if (s > 64) {
        if (q >> (128 - s)) return 0;               /* q * 2^s is past 2^128 */
        b.hi = q << (s - 64);
        b.lo = 0;
    } else if (s == 64) {
        b.hi = q;
        b.lo = 0;
    } else {
        b.hi = s ? q >> (64 - s) : 0;
        b.lo = q << s;
    }
    return a.hi != b.hi ? a.hi > b.hi : a.lo >= b.lo;
}

/* JS's toFixed: `v` to `f` decimals, the n with n / 10^f nearest the double's EXACT value, the
   larger on a tie, a minus whenever v < 0 (ECMA-262 Number.prototype.toFixed) -- not the value
   times 10^f, whose own rounding moves the tie: 1.325 is 1.32499999999999995559..., "1.32" in
   JS, while 1.325 * 100 rounds to 132.5 exactly. Integer arithmetic on the double's mantissa,
   for f <= 4 and |v| * 10^f below 2^62, so r, 2 * (r + 1) and m * p * 2 all fit; a value outside
   that (no figure the panel shows) takes the rounded product. */
static void js_fixed(char* d, size_t n, double v, int f)
{
    const double a = fabs(v);
    const char* sign = v < 0 ? "-" : "";
    uint64_t p = 1, r;
    int i, ex;
    for (i = 0; i < f; i++) p *= 10;
    if (f > 4 || !(a * (double)p < 4611686018427387904.0)) {
        snprintf(d, n, "%s%.*f", sign, f, floor(a * (double)p + 0.5) / (double)p);
        return;
    }
    if (a == 0) r = 0;
    else {
        const double fr = frexp(a, &ex);            /* a = fr * 2^ex, fr in [0.5, 1) */
        const uint64_t m = (uint64_t)ldexp(fr, 53); /* a = m * 2^(ex - 53), exactly  */
        const int s = 53 - ex;
        if (s <= 0) r = (uint64_t)a * p;            /* an integer */
        else {
            r = (uint64_t)floor(a * (double)p);
            while (r > 0 && !js_ge_half(m, s, p, 2 * r)) r--;      /* r <= a * p, exactly       */
            while (js_ge_half(m, s, p, 2 * (r + 1))) r++;          /* ... and a * p < r + 1     */
            if (js_ge_half(m, s, p, 2 * r + 1)) r++;               /* the half, the larger n    */
        }
    }
    if (f == 0) snprintf(d, n, "%s%llu", sign, (unsigned long long)r);
    else snprintf(d, n, "%s%llu.%0*llu", sign, (unsigned long long)(r / p), f, (unsigned long long)(r % p));
}

/* wiki.js techPanel's `f`: from ten thousand, whole thousands with "k"; else the rounded value with
   thousands separated by commas, as toLocaleString('en') */
static void js_amount(char* d, size_t n, double v)
{
    if (v >= 10000) { snprintf(d, n, "%.0fk", floor(v / 1000 + 0.5)); return; }
    {
        /* Math.round, then every three digits grouped; a value past 2^53 is not an integer's
           digits, and is set whole */
        const double r = floor(v + 0.5);
        char g[32], *o = g + sizeof g - 1;
        unsigned long long q;
        int k = 0;
        if (!(fabs(r) < 9007199254740992.0)) { snprintf(d, n, "%.0f", r); return; }
        q = (unsigned long long)fabs(r);
        *o = 0;
        do {
            if (k && k % 3 == 0) *--o = ',';
            *--o = (char)('0' + q % 10);
            q /= 10;
            k++;
        } while (q);
        snprintf(d, n, "%s%s", r < 0 ? "-" : "", o);
    }
}

/* one line of `s` in role `role` at (x, top), clipped; returns its width (the intrinsic,
   rounded up to a sixty-fourth) */
static double say(C* c, int role, const char* s, double x, double top, int ink, const UIR* clip, int emit)
{
    UITS t = ts(c, role, c->l->g.line);
    const int n = ui_pens(&c->u, &t, s, strlen(s));
    if (emit && n) ui_line(&c->u, &t, s, 0, n, x, top, ink, clip);
    return ui_ceil64(c->u.pen[n]);
}

/* the chip's height (#unit .chip: its padding, the picture, its label's line) */
static double chip_h(const C* c)
{
    return U(c, 1) + U(c, 24) + ts(c, R_CHIP, c->l->g.line).L + U(c, 1);
}

/* A chip for unit `k` at (x, y): its picture over its label, the label cut with three periods to
   the chip's width; its outline by what the unit is (.chip.up, .has, .sel). Returns its width. */
static double chip(C* c, int k, double x, double y, const UIR* clip, int emit)
{
    const WIKITECHTREE* T = c->st->tech;
    const double pad = U(c, 1), cw = U(c, 30), pic = U(c, 24), h = chip_h(c);
    if (emit && (!clip || (y < clip->y + clip->h && y + h > clip->y))) {
        const WIKITECHUNIT* x0 = &T->t->unit[T->unit[k]];
        UITS t = ts(c, R_CHIP, c->l->g.line);
        char title[WIKITECH_TITLE * 3 + 4], key[HUD_KEYLEN];
        const UIR pr = ui_rect(x + pad + ui_half(cw - pic), y + pad, pic, pic);
        const int ink = ui_light(&c->u, c->l->part[P_PANEL]);
        int n, m = 0;
        double tw;
        snprintf(key, sizeof key, "unit:%d", k);
        ui_hot(&c->u, ui_rect(x, y, cw + 2 * pad, h), clip, key);
        ui_solid(&c->u, HOP_SOLID, pr, 0, 1, clip);
        if (x0->pic >= 0) ui_pic(&c->u, x0->pic, pr, clip);
        if (k == c->st->tech_sel) frame_px(c, ui_rect(pr.x - 2, pr.y - 2, pr.w + 4, pr.h + 4), 2, 0, SEL_RGB, 0, clip);
        else if (T->upgrade[k]) frame_px(c, pr, 1, 0, UP_RGB, 1, clip);
        else if (T->upgrades[k].n) frame_px(c, pr, 1, 0, UP_RGB, 0, clip);
        unit_title(T, k, title, sizeof title);
        n = ui_pens(&c->u, &t, title, strlen(title));
        tw = ui_ceil64(c->u.pen[n]);
        if (tw <= cw) {
            if (n) ui_line(&c->u, &t, title, 0, n, x + pad + ui_half(cw - tw), y + pad + pic, ink, clip);
        } else {
            /* wiki.js chipCut: the most characters that fit with the three periods, then them */
            char cut[sizeof title + 4];
            int off[WIKITECH_TITLE * 3 + 4], k2;
            for (k2 = 0; k2 <= n && k2 < (int)(sizeof off / sizeof *off); k2++) off[k2] = c->u.off[k2];
            for (m = n - 1; m > 0; m--) {
                memcpy(cut, title, (size_t)off[m]);
                memcpy(cut + off[m], S_ELLIPSIS, sizeof S_ELLIPSIS);
                if (ui_ceil64(c->u.pen[ui_pens(&c->u, &t, cut, strlen(cut))]) <= cw) break;
            }
            memcpy(cut, title, (size_t)off[m > 0 ? m : 0]);
            memcpy(cut + off[m > 0 ? m : 0], S_ELLIPSIS, sizeof S_ELLIPSIS);
            n = ui_pens(&c->u, &t, cut, strlen(cut));
            ui_line(&c->u, &t, cut, 0, n, x + pad + ui_half(cw - ui_ceil64(c->u.pen[n])), y + pad + pic, ink, clip);
        }
    }
    return cw + 2 * pad;
}

/* `n` chips of order indices `ks` as a wrapping flex row, `w` wide at (x, y), the chips' gap between,
   with `sep` (the walk's ») between each two; returns its height */
static double chips(C* c, const int32_t* ks, int n, int sep, double x, double y, double w, const UIR* clip, int emit)
{
    const double gap = U(c, 1), ch = chip_h(c), cw = U(c, 32);
    const UITS t = ts(c, R_TEXT, c->l->g.line);
    const double sw = sep ? say(c, R_TEXT, S_SEP, 0, 0, 0, NULL, 0) : 0;
    const int ink = ui_light(&c->u, c->l->part[P_PANEL]);
    double cx = 0, cy = 0;
    int i;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        if (sep && i) {
            if (cx > 0 && cx + sw > w) { cx = 0; cy += ch + gap; }
            if (emit) {
                ui_group_begin(&c->u, 0.6);
                say(c, R_TEXT, S_SEP, x + cx, y + cy + ui_half(ch - t.L), ink, clip, 1);
                ui_group_end(&c->u);
            }
            cx += sw + gap;
        }
        if (cx > 0 && cx + cw > w) { cx = 0; cy += ch + gap; }
        chip(c, ks[i], x + cx, y + cy, clip, emit);
        cx += cw + gap;
    }
    return cy + ch;
}

/* an h4 of the panel (the category role) and its margin under it: returns its height */
static double unit_h4(C* c, const char* s, double x, double y, double w, const UIR* clip, int emit)
{
    return block(c, R_CATEGORY, c->l->g.line, s, x, y, w, ui_light(&c->u, c->l->part[P_PANEL]), clip, emit) + U(c, 1.5);
}

/* the list `L` of `T` as order indices into `out` */
static int unit_list(const WIKITECHTREE* T, WTLIST L, int32_t* out)
{
    uint32_t q;
    for (q = 0; q < L.n && q < UNIT_MAXLIST; q++) out[q] = T->pool[L.at + q];
    return (int)q;
}

/* #unit .uh: the picture, then the name, description and flags taking what is left, then the close
   button at the row's top; centred in the row's height. Returns its height. */
static double unit_head(C* c, int k, double x, double y, double w, const UIR* clip, int emit)
{
    const WIKITECHTREE* T = c->st->tech;
    const WIKITECHUNIT* x0 = &T->t->unit[T->unit[k]];
    const double pic = U(c, 32), gap = U(c, 4), bw = U(c, 17), bh = U(c, c->l->g.ctl.h);
    const double dw = fmax(0, w - pic - gap - gap - bw), fgap = U(c, 2), fpad = U(c, 2);
    const int ink = ui_light(&c->u, c->l->part[P_PANEL]);
    const uint32_t frgb = unit_ink_rgb(c, R_TEXT);
    char title[WIKITECH_TITLE * 3 + 4], desc[WIKITECH_DESC * 2 + 4];
    const char* flag[5];
    int up[5], nf = 0, i, pass;
    double th, dh, fh = 0, h, dy, L = ts(c, R_TEXT, c->l->g.line).L;
    unit_title(T, k, title, sizeof title);
    tagpu_wikitech_utf8(desc, sizeof desc, x0->desc, 0);
    flag[nf] = tagpu_wikitech_kind_name(T->kind[k]); up[nf++] = T->upgrade[k] != 0;
    if (T->upgradable[k]) { flag[nf] = "Upgradable"; up[nf++] = 1; }
    if (x0->cloaked) { flag[nf] = "Cloaked"; up[nf++] = 0; }
    if (x0->canfly) { flag[nf] = "Flies"; up[nf++] = 0; }
    if (x0->water) { flag[nf] = "Naval"; up[nf++] = 0; }
    th = block(c, R_ENTRY_TITLE, c->l->g.line, title, 0, 0, dw, ink, NULL, 0);
    dh = block(c, R_SUBTITLE, c->l->g.line, desc, 0, 0, dw, ink, NULL, 0);
    /* the flags wrap, the flags' gap across and down; each a 1 px border round its padded text */
    for (pass = 0; pass < 2; pass++) {
        double fx = 0, fy = 0;
        h = fmax(fmax(pic, th + dh + U(c, 1) + fh), bh);
        dy = y + ui_half(h - (th + dh + U(c, 1) + fh));
        for (i = 0; i < nf; i++) {
            const double fw = 1 + fpad + say(c, R_TEXT, flag[i], 0, 0, 0, NULL, 0) + fpad + 1;
            if (fx > 0 && fx + fw > dw) { fx = 0; fy += L + 2 + fgap; }
            if (pass && emit) {
                const double ax = x + pic + gap + fx, ay = dy + th + dh + U(c, 1) + fy;
                const uint32_t rgb = up[i] ? UP_RGB : frgb;
                int n0;
                ui_group_begin(&c->u, 0.85);
                frame_px(c, ui_rect(ax, ay, fw, L + 2), 1, 0, rgb, 0, clip);
                n0 = c->u.o->n;
                say(c, R_TEXT, flag[i], ax + 1 + fpad, ay + 1, ink, clip, 1);
                if (up[i]) recolour(c, n0, UP_RGB);
                ui_group_end(&c->u);
            }
            fx += fw + fgap;
        }
        fh = nf ? fy + L + 2 : 0;
    }
    if (emit) {
        const UIR pr = ui_rect(x, y + ui_half(h - pic), pic, pic);
        ui_solid(&c->u, HOP_SOLID, pr, 0, 1, clip);
        if (x0->pic >= 0) ui_pic(&c->u, x0->pic, pr, clip);
        block(c, R_ENTRY_TITLE, c->l->g.line, title, x + pic + gap, dy, dw, ink, clip, 1);
        block(c, R_SUBTITLE, c->l->g.line, desc, x + pic + gap, dy + th, dw, ink, clip, 1);
        {
            const UIR b = ui_rect(x + w - bw, y, bw, bh);
            const int part = c->l->part[!strcmp(c->st->held, "uclose") ? P_BUTTON_DOWN : P_BUTTON];
            if (!clip || (b.y < clip->y + clip->h && b.y + b.h > clip->y)) {
                ui_art(&c->u, part, b, clip);
                ui_hot(&c->u, b, clip, "uclose");
                ui_glyph_in(&c->u, c->l->icon_close, b, clip);
            }
        }
    }
    return h;
}

/* #unit .scr's column, the entry's gap between its items */
static double unit_rows(UI* u, const UIPANEL* p, double w, int emit, void* user)
{
    C* c = (C*)user;
    const WG* g = &c->l->g;
    const WIKITECHTREE* T = c->st->tech;
    const int k = c->st->tech_sel, ink = ui_light(&c->u, c->l->part[P_PANEL]);
    const WIKITECHUNIT* x0 = &T->t->unit[T->unit[k]];
    /* the body the view shows, whose actions it offers: an upgrade's first builder's */
    const WIKITECHUNIT* body = T->upgrade[k] && T->parents[k].n ? &T->t->unit[T->unit[T->pool[T->parents[k].at]]] : x0;
    const double gap = U(c, g->entry_gap), x = p->cx;
    const UIR* clip = &p->view;
    const double L = ts(c, R_TEXT, g->line).L;
    double y = p->cy;
    int32_t* ks = (int32_t*)malloc(UNIT_MAXLIST * sizeof *ks);
    char s[256], a[24], b[24];
    int i, n;
    (void)u;
    if (!ks) { c->u.o->overflow = 1; return 0; }

    y += unit_head(c, k, x, y, w, clip, emit) + gap;

    /* an upgrade has no body of its own: the view shows its first builder's */
    if (T->upgrade[k] && T->parents[k].n) {
        char bt[WIKITECH_TITLE * 3 + 4];
        unit_title(T, T->pool[T->parents[k].at], bt, sizeof bt);
        snprintf(s, sizeof s, "An upgrade has no body of its own. Shown is the %s that builds it.", bt);
        if (emit) ui_group_begin(&c->u, 0.8);
        y += block(c, R_TEXT, g->line, s, x, y, w, ink, clip, emit) + gap;
        if (emit) ui_group_end(&c->u);
    }

    /* the view (T4 draws the unit in it) and its actions, by what the FBI and the script say it can do */
    {
        const UIR vr = ui_rect(x, y, w, U(c, 120));
        static const char* const ACT[8] = { "Walk", "Take off", "Aim", "Fire", "Build", "On / off", "Turn", "Reset" };
        int on[8], na = 0;
        const char* lab[8];
        double ax = 0, ay = 0;
        const double ag = U(c, g->ctl_pair), bh = U(c, g->ctl.h);
        if (emit) {
            /* the note's line, unitview.js `say` (#unit .view .note: 2 units in, 1 up), clipped to
               the view: the 3D picture goes under it and over the ground (WIKIFRAME uview_cut) */
            static const char* const NOTE[TAGPU_TV_NOTES] = {
                "loading the model...", "drag to orbit \xC2\xB7 wheel to zoom \xC2\xB7 its own COB script poses it",
                "no script: the rest pose", "no model to draw",
                "nothing to draw: every piece with faces is hidden" };
            const int nk = c->st->tech_note >= 0 && c->st->tech_note < TAGPU_TV_NOTES ? c->st->tech_note : 0;
            const double nl = ts(c, R_TEXT, g->line).L;
            UIR vc = vr;
            if (vc.x < clip->x) { vc.w -= clip->x - vc.x; vc.x = clip->x; }
            if (vc.y < clip->y) { vc.h -= clip->y - vc.y; vc.y = clip->y; }
            if (vc.x + vc.w > clip->x + clip->w) vc.w = clip->x + clip->w - vc.x;
            if (vc.y + vc.h > clip->y + clip->h) vc.h = clip->y + clip->h - vc.y;
            ui_solid(&c->u, HOP_SOLID, vr, VIEW_RGB, 1, clip);
            ui_hot(&c->u, vr, clip, "uview");
            if (c->fr) {
                const double px = c->u.px;
                c->fr->uview.x = (float)(vr.x / px); c->fr->uview.y = (float)(vr.y / px);
                c->fr->uview.w = (float)(vr.w / px); c->fr->uview.h = (float)(vr.h / px);
                c->fr->uview_clip.x = (float)(clip->x / px); c->fr->uview_clip.y = (float)(clip->y / px);
                c->fr->uview_clip.w = (float)(clip->w / px); c->fr->uview_clip.h = (float)(clip->h / px);
                c->fr->uview_cut = c->u.o->n;
            }
            if (vc.w > 0 && vc.h > 0)
                say(c, R_TEXT, NOTE[nk], x + U(c, 2), vr.y + vr.h - U(c, 1) - nl, 0, &vc, 1);
        }
        y += vr.h + gap;
        const unsigned a = c->st->tech_act;
        if (body->mobile && !body->canfly) { lab[na] = ACT[0]; on[na++] = (a & TAGPU_TV_WALK) != 0; }
        if (body->canfly) { lab[na] = ACT[1]; on[na++] = (a & TAGPU_TV_FLY) != 0; }
        if (body->nweapon) { lab[na] = ACT[2]; on[na++] = (a & TAGPU_TV_AIM) != 0; lab[na] = ACT[3]; on[na++] = 0; }
        if (body->builder) { lab[na] = ACT[4]; on[na++] = (a & TAGPU_TV_BUILD) != 0; }
        if (body->structure && !body->nweapon) { lab[na] = ACT[5]; on[na++] = (a & TAGPU_TV_ON) != 0; }
        lab[na] = ACT[6]; on[na++] = (a & TAGPU_TV_TURN) != 0;
        lab[na] = ACT[7]; on[na++] = 0;
        for (i = 0; i < na; i++) {
            const double bw = ui_button_w(&c->u, &g->ctl, lab[i]);
            char key[HUD_KEYLEN];
            if (ax > 0 && ax + bw > w) { ax = 0; ay += bh + ag; }
            if (emit) {
                snprintf(key, sizeof key, "act:%s", lab[i]);
                ui_button_in(&c->u, &g->ctl, x + ax, y + ay, key, lab[i], -1,
                             c->l->part[on[i] || !strcmp(c->st->held, key) ? P_BUTTON_DOWN : P_BUTTON], 0, clip);
            }
            ax += bw + ag;
        }
        y += ay + bh + gap;
    }

    /* Figures: a grid of four columns, auto 1fr auto 1fr, a name and its value twice a row */
    {
        const char* name[16];
        char val[16][64];
        int nr = 0;
        double c1 = 0, c3 = 0, fr;
        const double cg = U(c, 5);
#define ROW(nm) (name[nr] = (nm), val[nr++])
        js_amount(ROW("Metal"), 64, x0->metal);
        js_amount(ROW("Energy"), 64, x0->energy);
        js_amount(ROW("Build time"), 64, x0->buildtime);
        js_amount(ROW("Health"), 64, x0->health);
        if (x0->speed) js_fixed(ROW("Speed"), 64, x0->speed, 2);
        snprintf(ROW("Sight"), 64, "%d", (int)x0->sight);
        if (x0->radar) snprintf(ROW("Radar"), 64, "%d", (int)x0->radar);
        if (x0->workertime) js_amount(ROW("Build power"), 64, x0->workertime);
        if (x0->energymake) js_fixed(ROW("Energy +"), 64, x0->energymake, 1);
        if (x0->extracts) js_fixed(ROW("Extracts"), 64, x0->extracts, 4);
        if (x0->metalmake) js_fixed(ROW("Metal +"), 64, x0->metalmake, 2);
        if (x0->mstore || x0->estore) {
            js_amount(a, sizeof a, x0->mstore);
            js_amount(b, sizeof b, x0->estore);
            snprintf(ROW("Storage"), 64, "%s / %s", a, b);
        }
        snprintf(ROW("Footprint"), 64, "%d \xC3\x97 %d", (int)x0->fx, (int)x0->fz);
        snprintf(ROW("Tier"), 64, "%d", T->tier[k]);
#undef ROW
        y += unit_h4(c, "Figures", x, y, w, clip, emit);
        for (i = 0; i < nr; i++) {
            const double nw = say(c, R_TEXT, name[i], 0, 0, 0, NULL, 0);
            if (i % 2) c3 = fmax(c3, nw);
            else c1 = fmax(c1, nw);
        }
        fr = fmax(0, (w - c1 - c3 - 3 * cg) / 2);
        for (i = 0; emit && i < nr; i++) {
            const double cx = x + (i % 2 ? c1 + cg + fr + cg : 0), ry = y + (i / 2) * L;
            ui_group_begin(&c->u, 0.7);
            say(c, R_TEXT, name[i], cx, ry, ink, clip, 1);
            ui_group_end(&c->u);
            say(c, R_TEXT, val[i], cx + (i % 2 ? c3 : c1) + cg, ry, ink, clip, 1);
        }
        y += ((nr + 1) / 2) * L + gap;
    }

    /* Weapons: each its name, and its range and damage at the row's right when the files know it */
    if (x0->nweapon) {
        y += unit_h4(c, "Weapons", x, y, w, clip, emit);
        for (i = 0; i < x0->nweapon && i < WIKITECH_WEAPONS; i++) {
            const WIKITECHWEAPON* wp = &x0->weapon[i];
            char nm[WIKITECH_WNAME * 2 + 4];
            tagpu_wikitech_utf8(nm, sizeof nm, (const unsigned char*)wp->name, 0);
            say(c, R_TEXT, nm, x, y, ink, clip, emit);
            if (wp->known) {
                snprintf(s, sizeof s, "range %d \xC2\xB7 damage %d", (int)wp->range, (int)wp->damage);
                say(c, R_TEXT, s, x + w - say(c, R_TEXT, s, 0, 0, 0, NULL, 0), y, ink, clip, emit);
            }
            y += L;
        }
        y += gap;
    }

    /* How to get it: the walk from the commander, › between */
    y += unit_h4(c, "How to get it", x, y, w, clip, emit);
    n = tagpu_wikitech_path(T, k, ks, UNIT_MAXLIST);
    y += chips(c, ks, n < 0 ? 0 : n, 1, x, y, w, clip, emit) + gap;

    if (T->parents[k].n) {
        snprintf(s, sizeof s, "Built by (%u)", T->parents[k].n);
        y += unit_h4(c, s, x, y, w, clip, emit);
        n = unit_list(T, T->parents[k], ks);
        y += chips(c, ks, n, 0, x, y, w, clip, emit) + gap;
    }
    if (T->upgrades[k].n) {
        snprintf(s, sizeof s, "Upgrades (%u)", T->upgrades[k].n);
        y += unit_h4(c, s, x, y, w, clip, emit);
        n = unit_list(T, T->upgrades[k], ks);
        y += chips(c, ks, n, 0, x, y, w, clip, emit) + gap;
    }
    if (T->builds[k].n) {
        int kind;
        snprintf(s, sizeof s, "Builds (%u)", T->builds[k].n);
        y += unit_h4(c, s, x, y, w, clip, emit);
        for (kind = 0; kind < WT_KINDS; kind++) {
            uint32_t q;
            n = 0;
            for (q = 0; q < T->builds[k].n && n < UNIT_MAXLIST; q++) {
                const int32_t v = T->pool[T->builds[k].at + q];
                if (T->kind[v] == kind) ks[n++] = v;
            }
            if (!n) continue;
            y += unit_h4(c, tagpu_wikitech_kind_name(kind), x, y, w, clip, emit);
            y += chips(c, ks, n, 0, x, y, w, clip, emit);
        }
        y += gap;
    }
    free(ks);
    /* the column's height: the last item's bottom, without a gap after it */
    return y - gap - p->cy;
}

/* #unit: the panel over the stage's right at `box` (About's place), padded as About is; its column
   scrolls inside the padding (scroll:unit) */
static void unit_panel(C* c, UIR box)
{
    const WG* g = &c->l->g;
    const double pt = U(c, g->pnl.pn[0] + g->entry_pad[0]), pr = U(c, g->pnl.pn[1] + g->entry_pad[1]);
    const double pb = U(c, g->pnl.pn[2] + g->entry_pad[0]), pl = U(c, g->pnl.pn[3] + g->entry_pad[1]);
    UIPANELG sg;
    UISCROLL a;
    memset(&sg, 0, sizeof sg);
    sg.scrollbar = g->pnl.scrollbar;
    ui_art(&c->u, c->l->part[P_PANEL], box, NULL);
    ui_panel(&c->u, &sg, -1, ui_rect(box.x + pl, box.y + pt, fmax(0, box.w - pl - pr), fmax(0, box.h - pt - pb)),
             c->st->scroll[WIKI_SCROLL_UNIT], unit_rows, c, "scroll:unit", &a);
    c->fr->scroll[WIKI_SCROLL_UNIT] = a.scroll;
    c->fr->scroll_max[WIKI_SCROLL_UNIT] = a.scroll_max;
    c->fr->track[WIKI_SCROLL_UNIT] = a.track;
    c->fr->thumb_y[WIKI_SCROLL_UNIT] = a.thumb_y;
    c->fr->thumb_h[WIKI_SCROLL_UNIT] = a.thumb_h;
}

/* The theatre in the window's body: the tree's column, as wide as on the normal page and the body's
   whole height, with the entry's demos under its row (tree_rows); beside it, the layout's gap
   between, a grid column of the stage's bezel, which takes what the others leave, the row and the
   caption; then About or the unit's panel over the stage, built last of all (see below).

   THE ROW (#t-row) is a flex row, the controls' gap between its items: the badge when locked, the
   demo's title, and the controls with About. The title is the one item that grows and
   shrinks (flex 1 1 0): it takes the row's free space and clips its line to it, so every control
   keeps its size and place whatever the title. `row_cut` says the fixed part did not fit.

   ABOUT'S AND THE UNIT PANEL'S ARE THE PAGE'S ONLY HOPF_TOP OPS, so they are a layer of their own,
   which the wiki's pass in tagpu_vk_hud.c draws last, after the stage and the stage's HUD that it
   lies over, and which the hit map (tagpu_wikistate.c) puts before every other control. The
   theatre only places it (`top_box`, `top_kind`) and sets the cover; tagpu_wikilay_build builds it
   after the window and the wiki button, so the one list whose ops and hits grow with a mod's
   menus is the last into the frame's bounds: if a panel ever overflowed them, what it dropped
   would be its own, never the window's controls. */
static void theatre(C* c, UIR body)
{
    const WG* g = &c->l->g;
    const WIKIENTRY* e = entry(c);
    const double gap = U(c, g->gap), row_h = U(c, g->ctl.h), cap_h = U(c, g->caption_h);
    const double cg = U(c, g->ctl_gap), t0 = ui_lu(body.w * g->col[0] / 100);
    const int about = c->st->about != 0, wink = ui_light(&c->u, c->l->part[P_WINDOW]);
    const int lk = locked(c, c->st->entry) || c->st->sim_locked, tech = e && e->techtree;
    const UIR sbox = ui_rect(body.x + t0 + gap, body.y, fmax(0, body.w - t0 - gap), body.h - gap - row_h - gap - cap_h);
    const double ry = sbox.y + sbox.h + gap;
    double fixed, x = sbox.x, tw;

    panel(c, WIKI_SCROLL_TREE, ui_rect(body.x, body.y, t0, body.h), tree_rows, "scroll:tree");
    stage_in(c, sbox);

    /* the row: what its fixed items take, then the title's share */
    fixed = (lk ? ui_badge_w(&c->u, &c->l->g.bdg, S_BADGE) : 0) + (tech ? tech_controls(c, 0, 0, 0) : controls(c, 0, 0, 1, 0)) +
            cg * (1 + lk);
    tw = fmax(0, sbox.w - fixed);
    c->fr->row_cut = fixed > sbox.w;
    if (lk) x += badge(c, x, ry + ui_half(row_h - U(c, g->bdg.h))) + cg;
    if (tech || has_demo(c)) {
        /* #t-title: one line, nowrap, its overflow hidden; centred in the row's height. The tech
           tree's is the tree's own (wiki.js drawTech) */
        const char* s = tech ? c->st->tech_title : filled_or(c, FILL_ROW, e, e->demo[c->st->demo].title, e->demo[c->st->demo].id);
        UITS t = ts(c, R_DEMO, g->line);
        int n = ui_pens(&c->u, &t, s, strlen(s));
        UIR clip = ui_rect(x, ry + ui_half(row_h - t.L), tw, t.L);
        ui_line(&c->u, &t, s, 0, n, x, clip.y, wink, &clip);
    }
    x += tw + cg;
    if (tech) tech_controls(c, x, ry, 1);
    else controls(c, x, ry, 1, 1);

    caption(c, sbox.x, ry + row_h + gap, sbox.w);

    {
        /* About, or the tech tree's unit panel, in one place over the stage's right: never both;
           its share of the bezel, at most theatre.about_max units (#about, #unit max-width) */
        const double wa = fmin(ui_lu(sbox.w * g->theatre_about / 100), U(c, g->theatre_about_max));
        const UIR pb = ui_rect(sbox.x + sbox.w - U(c, g->bz[1]) - wa, sbox.y + U(c, g->bz[0]), wa,
                               sbox.h - U(c, g->bz[0]) - U(c, g->bz[2]));
        const int unit = !about && tech && c->st->tech && c->st->tech_sel >= 0 && c->st->tech_sel < c->st->tech->n;
        c->top_kind = about ? TOP_ABOUT : unit ? TOP_UNIT : TOP_NONE;
        c->top_box = pb;
        if (about || unit) c->fr->cover = (float)(pb.x / c->u.px);
    }
}

/* ---- the window (#win): its part, title and close, and the body's three columns --------------- */

static void window_(C* c)
{
    const WG* g = &c->l->g;
    const double W = c->st->W, H = c->st->H;
    UIR win, wt, wx, body;
    double t0, t1, t2, gap;
    win = ui_rect(ui_lu((W - W * g->share / 100) / 2), ui_lu((H - H * g->share / 100) / 2), ui_lu(W * g->share / 100), ui_lu(H * g->share / 100));
    c->fr->window.x = (float)(win.x / c->u.px); c->fr->window.y = (float)(win.y / c->u.px);
    c->fr->window.w = (float)(win.w / c->u.px); c->fr->window.h = (float)(win.h / c->u.px);
    ui_hot(&c->u, win, NULL, "window");
    ui_art(&c->u, c->l->part[P_WINDOW], win, NULL);
    /* the title in its groove: its own ink, the style's whatever the window's face */
    wt = ui_rect(win.x + U(c, g->wt[0]), win.y + U(c, g->wt[2]), win.w - U(c, g->wt[0]) - U(c, g->wt[1]), U(c, g->wt[3]));
    {
        UITS t = ts(c, R_WINDOW_TITLE, g->line);
        int n = ui_pens(&c->u, &t, S_TITLE, strlen(S_TITLE));
        ui_line(&c->u, &t, S_TITLE, 0, n, wt.x + U(c, g->title_pad), wt.y + ui_half(wt.h - t.L), 0, &wt);
    }
    /* the close button in its bay */
    wx = ui_rect(win.x + win.w - U(c, g->wx[0]) - U(c, g->wx[2]), win.y + U(c, g->wx[1]), U(c, g->wx[2]), U(c, g->wx[3]));
    ui_art(&c->u, c->l->part[!strcmp(c->st->held, "close") ? P_CLOSE_DOWN : P_CLOSE], wx, NULL);
    ui_hot(&c->u, wx, NULL, "close");
    ui_glyph(&c->u, c->l->icon_close, wx);
    /* the body: a frame:game entry's theatre, or a grid of the two shares and the rest, its gaps
       between */
    body = ui_rect(win.x + U(c, g->wb[3]), win.y + U(c, g->wb[0]), win.w - U(c, g->wb[3]) - U(c, g->wb[1]),
                win.h - U(c, g->wb[0]) - U(c, g->wb[2]));
    if (entry(c) && (entry(c)->frame == WIKI_FRAME_GAME || entry(c)->techtree)) {
        theatre(c, body);
        return;
    }
    gap = U(c, g->gap);
    t0 = ui_lu(body.w * g->col[0] / 100);
    t1 = ui_lu(body.w * g->col[1] / 100);
    t2 = fmax(0, body.w - t0 - t1 - 2 * gap);
    panel(c, WIKI_SCROLL_TREE, ui_rect(body.x, body.y, t0, body.h), tree_rows, "scroll:tree");
    panel(c, WIKI_SCROLL_DEMOS, ui_rect(body.x + t0 + gap, body.y, t1, body.h), demo_rows, "scroll:demos");
    entry_col(c, ui_rect(body.x + t0 + gap + t1 + gap, body.y, t2, body.h));
}

/* ---- the top bar's buttons, in its own pixels: the wiki button (#b-wiki), left of the rewind
   button or in its slot, down while the window is open; and the Config sprocket (#b-gear) in the
   engine's slot for it (tagpu_menu.c, trigger_rect), when the page says this frame draws it. Each is
   an emblem with no plate: its part is the whole button, its _down part the whole button pressed.
   THE SPROCKET IS DRAWN, NOT A CONTROL: its press and release are tagpu_menu.c's, on the engine's
   rect, so it puts nothing in the wiki's hit map and a press on it reaches the menu as before. --- */

static void wiki_button(C* c)
{
    const WG* g = &c->l->g;
    const double k = c->st->bar / 32;
    const double side = U(c, g->btn_side * k), top = U(c, g->btn_top * k);
    UIR b = ui_rect(c->st->W - U(c, g->btn_right[c->st->rewind ? 0 : 1] * k) - side, top, side, side);
    int down = c->st->open || !strcmp(c->st->held, "wiki");
    ui_art(&c->u, c->l->part[down ? P_WIKI_DOWN : P_WIKI], b, NULL);
    ui_hot(&c->u, b, NULL, "wiki");
    if (c->st->config)
        ui_art(&c->u, c->l->part[c->st->config == 2 ? P_CONFIG_DOWN : P_CONFIG],
            ui_rect(c->st->W - U(c, g->btn_config * k) - side, top, side, side), NULL);
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

int tagpu_wikilay_build(const WIKILAY* l, const WIKIDATA* d, const UIFACES* f, const WIKIPAGE* st,
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
    /* WIKI_MAXHOT, the bound: H is 720 units, so a list's scroller is under 720 units tall, and a row
       with its gap after it is at least layout.json's row.h + row.after, 17.5 today, which puts at
       most 42 rows of each list on screen -- 84 -- beside the window's own 11 controls and the
       button. The tech tree's unit panel adds its chips and its 11 controls (the close, the view,
       at most 8 actions, the scroller): a chip is 32 units wide with 1 between, in a column at
       most theatre.about_max (400) wide, so at most 12 a row; it is at least 26 units tall with
       1 between, so at most (720 + 2 * 26 + 1) / 27 < 29 rows of chips meet the scroller's view
       -- 336 chips. 84 + 12 + 336 + 11 = 443: under WIKI_MAXHOT, so no control on screen goes
       without its hit, at any aspect. */
    ui_begin(&c->u, out, f, l->style, l->light, (double)st->H / 720, WIKI_MAXHOT);
    c->l = l; c->d = d; c->st = st; c->fr = fr;
    memset(fr, 0, sizeof *fr);
    out->n = out->nrun = out->ntext = out->overflow = out->ngrp = 0;
    out->first = out->last = 0;
    out->track_x0 = out->track_x1 = 0;
    c->top_kind = TOP_NONE;
    if (st->open) window_(c);
    wiki_button(c);
    if (c->top_kind != TOP_NONE) {
        c->u.top = 1;
        if (c->top_kind == TOP_ABOUT) about_panel(c, c->top_box);
        else unit_panel(c, c->top_box);
        c->u.top = 0;
    }
    free(c);
    return 1;
}

/* one state of the check built: 0, with the state named in `err`, when the build refuses or drops
   an op, a run, a text, a hit or a group (HUDOPS.overflow), or, in the theatre, when its stage is
   shorter than theatre.stage_min or its row's fixed part is wider than the stage's bezel */
static int check_build(const WIKILAY* l, const WIKIDATA* d, const UIFACES* f, const WIKIPAGE* st,
                       HUDOPS* scratch, WIKIFRAME* fr, char* err, size_t errn)
{
    const int theatre_ = st->open && st->entry >= 0 && st->entry < d->nentry &&
                         (d->entry[st->entry].frame == WIKI_FRAME_GAME || d->entry[st->entry].techtree);
    if (!tagpu_wikilay_build(l, d, f, st, scratch, fr, err, errn)) return 0;
    if (scratch->overflow) {
        if (err && errn)
            snprintf(err, errn, "wiki layout: the page overflows its ops at %.0fx%.0f (%s, entry %d, demo %d)",
                     st->W, st->H, st->open ? "open" : "closed", st->entry, st->demo);
        return 0;
    }
    if (theatre_ && fr->stage.h < l->g.theatre_min) {
        if (err && errn)
            snprintf(err, errn, "wiki layout: the theatre's stage is %.1f units tall at %.0fx%.0f, below theatre.stage_min %g",
                     fr->stage.h, st->W, st->H, l->g.theatre_min);
        return 0;
    }
    if (theatre_ && fr->row_cut) {
        if (err && errn)
            snprintf(err, errn, "wiki layout: the theatre's row of %s, %d demos, is wider than the stage's bezel at %.0fx%.0f",
                     d->entry[st->entry].id, d->entry[st->entry].ndemo, st->W, st->H);
        return 0;
    }
    return 1;
}

/* THE THEATRE'S OWN STATES of game entry `e`, from the open page `base`: About open with the text
   scrolled to its limit, the tree scrolled to its limit (the build holds a scroll to its panel's),
   Try it yourself, and locked with either label of Try it, the widest row a theatre can have */
static int check_theatre(const WIKILAY* l, const WIKIDATA* d, const UIFACES* f, const WIKIPAGE* base, int e,
                         HUDOPS* scratch, char* err, size_t errn)
{
    WIKIPAGE st = *base;
    WIKIFRAME fr;
    st.entry = e;
    st.demo = 0;
    st.caption[0] = 0;
    st.playing = 1;
    st.done = 0;
    st.speed = 1;
    st.about = 1;
    st.scroll[WIKI_SCROLL_TEXT] = 1e9f;
    if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
    st.about = 0;
    st.scroll[WIKI_SCROLL_TREE] = 1e9f;
    if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
    st.try_it = 1;
    if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
    st.sim_locked = 1;
    if (!check_build(l, d, f, &st, scratch, &fr, err, errn)) return 0;
    st.try_it = 0;
    return check_build(l, d, f, &st, scratch, &fr, err, errn);
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
static int check_states(const WIKILAY* l, const WIKIDATA* d, const UIFACES* f, const WIKICASTING* k, const char* who,
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
        st.config = 2;          /* the sprocket's op in every state, as a frame that draws it has */
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
            if ((en->frame == WIKI_FRAME_GAME || en->techtree) && !check_theatre(l, d, f, &st, e, scratch, err, errn)) return 0;
        }
    }
    return 1;
}

int tagpu_wikilay_check(const WIKILAY* l, const WIKIDATA* d, const UIFACES* f,
                        const WIKICASTING* castings, int ncasting, HUDOPS* scratch,
                        char* err, size_t errn)
{
    /* the icons the window and the stage draw: the build draws a button without an icon it lacks,
       so their absence is refused here rather than drawn as a blank control */
    static const char* const ICON[] = { "i_play", "i_pause", "i_replay", "i_open", "i_shut",
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
        if (ui_find_part(l->kit, ICON[i]) < 0) {
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

static int read_layout(WIKILAY* l, const TJV* L, char* err, size_t errn)
{
    UILAYOUT x;
    WG* g = &l->g;
    const TJV* kit = tj_get(tj_get(L, "geom"), "kit");
    int i;
    x.L = L; x.bad = 0; x.err = err; x.errn = errn; x.what = "wiki layout";
    g->btn_side = ui_num(&x, "button", "side", -1);
    g->btn_top = ui_num(&x, "button", "top", -1);
    for (i = 0; i < 2; i++) g->btn_right[i] = ui_num(&x, "button", "right", i);
    g->btn_config = ui_num(&x, "button", "config", -1);
    g->share = ui_num(&x, "window", "share", -1);
    g->font = ui_num(&x, "window", "font", -1);
    g->line = ui_num(&x, "window", "line", -1);
    g->row.line = g->ctl.line = g->bdg.line = g->line;
    {
        UILAYOUT k = x;
        k.L = kit;
        if (!tj_is(kit, TJ_OBJ) && !x.bad++) snprintf(err, errn, "wiki layout: geom.kit is missing");
        for (i = 0; i < 4; i++) {
            g->wt[i] = ui_num(&k, "wt", NULL, i); g->wx[i] = ui_num(&k, "wx", NULL, i); g->wb[i] = ui_num(&k, "wb", NULL, i);
            g->pnl.pn[i] = ui_num(&k, "pn", NULL, i); g->bz[i] = ui_num(&k, "bz", NULL, i);
        }
        g->gap = ui_num(&k, "gap", NULL, -1);
        x.bad += k.bad;
    }
    g->title_pad = ui_num(&x, "title", "pad", -1);
    for (i = 0; i < 2; i++) g->col[i] = ui_num(&x, "columns", NULL, i);
    for (i = 0; i < 2; i++) g->pnl.pad[i] = ui_num(&x, "panel", "pad", i);
    g->pnl.scrollbar = ui_num(&x, "scrollbar", NULL, -1);
    g->indent = ui_num(&x, "tree", "indent", -1);
    g->mark = ui_num(&x, "tree", "mark", -1);
    g->row.h = ui_num(&x, "row", "h", -1);
    for (i = 0; i < 2; i++) g->row.pad[i] = ui_num(&x, "row", "pad", i);
    g->row.gap = ui_num(&x, "row", "gap", -1);
    g->row.after = ui_num(&x, "row", "after", -1);
    g->row.lock = ui_num(&x, "lock", NULL, -1);
    g->demo_num = ui_num(&x, "demo", "num", -1);
    for (i = 0; i < 2; i++) g->entry_pad[i] = ui_num(&x, "entry", "pad", i);
    g->entry_gap = ui_num(&x, "entry", "gap", -1);
    g->title_line = ui_num(&x, "entry", "title_line", -1);
    g->bdg.h = ui_num(&x, "badge", "h", -1);
    g->bdg.pad = ui_num(&x, "badge", "pad", -1);
    g->bdg.gap = ui_num(&x, "badge", "gap", -1);
    g->bdg.dot = ui_num(&x, "badge", "dot", -1);
    g->stage_min = ui_num(&x, "stage", "min", -1);
    g->caption_h = ui_num(&x, "strips", "caption", -1);
    g->keys_h = ui_num(&x, "strips", "keys", -1);
    g->keys_gap = ui_num(&x, "keys", "gap", -1);
    g->keycap_h = ui_num(&x, "keycap", "h", -1);
    g->keycap_pad = ui_num(&x, "keycap", "pad", -1);
    g->ctl_gap = ui_num(&x, "controls", "gap", -1);
    g->ctl_pair = ui_num(&x, "controls", "pair", -1);
    g->ctl_try = ui_num(&x, "controls", "try", -1);
    g->ctl.h = ui_num(&x, "controls", "h", -1);
    g->ctl.pad = ui_num(&x, "controls", "pad", -1);
    g->ctl.min = ui_num(&x, "controls", "min", -1);
    g->text_share = ui_num(&x, "text", "share", -1);
    g->text_para = ui_num(&x, "text", "para", -1);
    g->theatre_about = ui_num(&x, "theatre", "about", -1);
    g->theatre_min = ui_num(&x, "theatre", "stage_min", -1);
    g->theatre_about_max = ui_num(&x, "theatre", "about_max", -1);
    if (!x.bad && !(g->theatre_about_max > 0)) {
        snprintf(err, errn, "wiki layout: theatre.about_max is not a width above 0");
        x.bad++;
    }
    if (!x.bad && !(g->theatre_about > 0 && g->theatre_about < 100)) {
        snprintf(err, errn, "wiki layout: theatre.about is not a share of the body above 0 and below 100");
        x.bad++;
    }
    if (!x.bad && !(g->theatre_min >= g->stage_min)) {
        snprintf(err, errn, "wiki layout: theatre.stage_min is below stage.min");
        x.bad++;
    }
    return !x.bad;
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
        if ((l->part[i] = ui_find_part(l->kit, PART[i])) < 0) { snprintf(err, errn, "wiki kit: no part '%s'", PART[i]); goto done; }
    l->icon_close = ui_find_part(l->kit, "i_close");
    if (l->icon_close < 0) l->icon_close = ui_find_part(l->kit, "i_x");
    l->icon_play = ui_find_part(l->kit, "i_play");
    l->icon_pause = ui_find_part(l->kit, "i_pause");
    l->icon_replay = ui_find_part(l->kit, "i_replay");
    l->icon_open = ui_find_part(l->kit, "i_open");
    l->icon_shut = ui_find_part(l->kit, "i_shut");
    l->icon_dot = ui_find_part(l->kit, "i_dot");
    l->nstyle = styles->nstyle;
    memcpy(l->style, styles->style, sizeof l->style);
    roles = tj_get(tagpu_json_root(doc), "roles");
    for (i = 0; i < R_N; i++) {
        const char* sn = tj_str(tj_get(roles, ROLE[i]), NULL);
        for (j = 0; sn && j < l->nstyle && strcmp(l->style[j].name, sn); j++) {}
        if (!sn || j >= l->nstyle) { snprintf(err, errn, "wiki layout: roles.%s names no HUD style", ROLE[i]); goto done; }
        l->role[i] = j;
    }
    l->g.ctl.style = l->role[R_CONTROL];
    l->g.bdg.style = l->role[R_BADGE];
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

void tagpu_wikilay_button(const WIKILAY* l, int rewind, double* side, double* top, double* right, double* config)
{
    *side = l->g.btn_side;
    *top = l->g.btn_top;
    *right = l->g.btn_right[rewind ? 0 : 1];
    *config = l->g.btn_config;
}

void tagpu_wikilay_light(WIKILAY* l, int part, int lt)
{
    if (l && part >= 0 && part < UI_MAXPART) l->light[part] = (unsigned char)(lt != 0);
}
