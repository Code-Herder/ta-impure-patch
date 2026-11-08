/* tagpu_modslay.c -- the Mods screen's window. Contract: tagpu_modslay.h.

   EACH FUNCTION BELOW IS ONE OF THE MOCKUP'S RULES (tools/mods/mockup/mods.css, mods.js): a box with
   the content it holds, emitted in the document's order. A rule changed in one place is changed in
   the other; modslaycheck.py is what notices when they disagree.

   The widgets, and the rounding and line rules the arithmetic rests on: tagpu_uikit.c. A control in
   a scrolling panel is drawn here over ui_art, ui_hot and ui_line because ui_button and ui_badge do
   not clip. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_modslay.h"
#include "tagpu_json.h"

/* the kit's parts the rules draw, by name; the load refuses a kit without one */
enum { P_WINDOW, P_CLOSE, P_CLOSE_DOWN, P_ICON_CLOSE, P_PANEL, P_ROW, P_ROW_SEL, P_BUTTON, P_BUTTON_DOWN, P_TAB, P_TAB_SEL, P_TAB_DOWN, P_CHECK_OFF, P_CHECK_ON,
       P_RADIO_OFF, P_RADIO_ON, P_TRACK, P_FILL, P_BADGE_ACTIVE, P_BADGE_UPDATE, P_BADGE_GREY, P_BADGE_CONFLICT, P_DOT, P_N };
static const char* const PART[P_N] = { "window", "close", "close_down", "i_close", "panel", "row", "row_selected", "button", "button_down", "tab", "tab_selected",
    "tab_down", "check_off", "check_on", "radio_off", "radio_on", "progress_track", "progress_fill", "badge_active",
    "badge_update", "badge_grey", "badge_conflict", "i_dot" };

/* layout.json `roles`, the text of each element */
enum { R_WINDOW_TITLE, R_TAB, R_NAME, R_DETAIL, R_ACT, R_BADGE, R_EMPTY, R_PANE_TITLE, R_META, R_SUMMARY, R_NOTE, R_HEADER,
       R_OPTION, R_REASON, R_VALUE, R_FOOTER, R_N };
static const char* const ROLE[R_N] = { "window_title", "tab", "name", "detail", "act", "badge", "empty", "pane_title", "meta",
    "summary", "note", "header", "option", "reason", "value", "footer" };

/* The mockup's strings (mods.js) and the colours its CSS gives outright (mods.css). modslaycheck.py
   compares every text, dot and solid, so one that drifts from the mockup's is named there. */
#define S_TITLE     "Mods"
#define S_BACK      "Back"
#define S_CONFIRM   "Confirm"
#define S_UPDATEALL "Update all"
#define S_INSTALL   "Install"
#define S_UPDATE    "Update"
#define S_OPTIONS   "Play options"
static const char* const TAB_LABEL[MODS_NTAB] = { "Manage", "Explore", "Updates" };
#define BACK_RGB    0x0B0C0Au       /* the backdrop: opaque, the engine's menu is under it */
#define OFF_A       0.45            /* a greyed row: a disabled button's (tagpu_uikit.c)   */
#define LINK_RGB    0xE2B84Au       /* a showcase's link: its run's colour, no underline   */
#define RULE_RGB    0xD8D8C8u       /* the line under a showcase's head ...                */
#define RULE_A      0.18            /* ... at this opacity                                 */

/* layout.json's numbers, in units unless named */
typedef struct {
    double share, line;             /* the window's share (percent), the text's line height */
    double wt[4], wx[4], wb[4], gap;       /* wt l r t h, wx r t w h, wb t r b l */
    UIPANELG pnl;
    double title_pad;
    UIBUTTONG tab, foot, act;
    double tab_gap, foot_gap;
    double col;                     /* the list's share of the body, percent */
    double list_h;
    double row_h, row_py, row_px, row_gap, row_after;
    double lock, radio, value, act_w;
    double badge_h, badge_pad;
    double prog_w, prog_h, prog_in;
    double title_line, para, head, head_after;
    /* the showcase: the logo's most height, its padding above and below, its most width in percent of the pane and over a
       backdrop; a backdrop's head's padding (top, sides, bottom) and the room under its logo; the rule's room above and
       below; the room under a banner; a list item's text inset, its bullet's side, the gap between columns and between rows */
    double logo_h, logo_pad[2], logo_w, logo_hero_w, hero_pad[3], hero_logo_after, rule[2], banner_after;
    double list_indent, list_bullet, list_gap, list_after;
} MG;

struct MODSLAY {
    const HUDLAY*   kit;
    MG              g;
    int             part[P_N];
    int             role[R_N];
    int             nstyle;
    struct HUDSTYLE style[24];
    unsigned char   light[UI_MAXPART];
};

typedef struct {
    UI              u;
    const MODSLAY*  l;
    MODSPAGE        st;             /* the page bounded (page_bound): every count, index, tag and text below is */
    MODSFRAME*      fr;
    int             cut;            /* a row's fixed part is wider than the row: its text has no room */
    char            join[MODS_SHOWTEXT + 8];   /* a paragraph and the link that ends it, set as one text */
} C;

/* THE BOUND. A page is values only, but it crosses threads on a copy, so nothing here trusts it: each
   count is held to its array, an index or a tag out of range reads as none, and the last byte of
   each text is a NUL, which is what makes the strlen of every text below safe. */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
#define TERM(a) ((a)[sizeof(a) - 1] = 0)
static void page_bound(MODSPAGE* s)
{
    int i;
    s->tab = s->tab >= 0 && s->tab < MODS_NTAB ? s->tab : MODS_TAB_MANAGE;
    s->nrow = clampi(s->nrow, 0, MODS_MAXROW);
    s->nopt = clampi(s->nopt, 0, MODS_MAXOPT);
    if (s->selected < 0 || s->selected >= s->nrow) s->selected = -1;
    for (i = 0; i < s->nrow; i++) {
        MODSROW* r = &s->row[i];
        TERM(r->name); TERM(r->detail); TERM(r->badge_text);
        if (r->badge > MODS_BADGE_CONFLICT) r->badge = MODS_BADGE_NONE;
        if (r->act > MODS_ACT_CHECK_ON) r->act = MODS_ACT_NONE;
        r->progress = (int16_t)clampi(r->progress, -1, 1000);
    }
    for (i = 0; i < s->nopt; i++) {
        MODSOPT* o = &s->opt[i];
        TERM(o->label); TERM(o->value); TERM(o->reason);
    }
    TERM(s->empty); TERM(s->title); TERM(s->meta); TERM(s->summary); TERM(s->note); TERM(s->held); TERM(s->confirm);
    /* a block's offsets each name a NUL-terminated string inside the text, the text's last byte being a NUL */
    TERM(s->show.id); TERM(s->show.name); TERM(s->show.version); TERM(s->show.by); TERM(s->show.site); TERM(s->show.text);
    s->show.nblock = clampi(s->show.nblock, 0, MODS_MAXBLOCK);
    for (i = 0; i < s->show.nblock; i++) {
        MODSBLOCK* b = &s->show.block[i];
        if (b->kind > MODS_BLOCK_LINK) b->kind = MODS_BLOCK_PARA;
        b->cols = (uint8_t)clampi(b->cols, 1, MODS_MAXCOLS);
        if (b->text >= MODS_SHOWTEXT) b->text = MODS_SHOWTEXT - 1;
        if (b->link >= MODS_SHOWTEXT) b->link = MODS_SHOWTEXT - 1;
    }
}

static double U(const C* c, double n) { return ui_len(&c->u, n); }
static int part(const C* c, int p) { return c->l->part[p]; }
static int held(const C* c, const char* key) { return key && key[0] && !strcmp(c->st.held, key); }
static int light(const C* c, int p) { return ui_light(&c->u, part(c, p)); }
static double block(C* c, int role, double lh, const char* s, double x, double y, double w, int ink, const UIR* clip, int emit)
{
    return ui_block(&c->u, c->l->role[role], lh, s, x, y, w, ink, clip, emit);
}
static UITS ts(const C* c, int role) { return ui_ts(&c->u, c->l->role[role], c->l->g.line); }

/* the browser skips what its scroller clips wholly away, each box on its own */
static int shows(UIR a, const UIR* k) { return !k || (a.y < k->y + k->h && a.y + a.h > k->y && a.w > 0 && k->h > 0); }
static void art(C* c, int p, UIR r, const UIR* clip) { if (shows(r, clip)) ui_art(&c->u, p, r, clip); }

/* a text's intrinsic width in a role */
static double text_w(C* c, int role, const char* s)
{
    UITS t = ts(c, role);
    return ui_ceil64(c->u.pen[ui_pens(&c->u, &t, s, strlen(s))]);
}
static void line_at(C* c, int role, const char* s, double x, double y, int ink, const UIR* clip)
{
    UITS t = ts(c, role);
    ui_line(&c->u, &t, s, 0, ui_pens(&c->u, &t, s, strlen(s)), x, y, ink, clip);
}

/* A button in a panel: ui_button's box and text, clipped, a NULL key making it no control. Its part
   down while held. Returns its width. */
static double button(C* c, const UIBUTTONG* g, double x, double y, const char* key, const char* label, const UIR* clip)
{
    UITS t = ui_ts(&c->u, g->style, g->line);
    const double w = ui_button_w(&c->u, g, label), h = ui_len(&c->u, g->h), pad = ui_len(&c->u, g->pad);
    const int p = held(c, key) ? P_BUTTON_DOWN : P_BUTTON, n = ui_pens(&c->u, &t, label, strlen(label));
    const double tw = ui_ceil64(c->u.pen[n]);
    const UIR b = ui_rect(x, y, w, h);
    art(c, part(c, p), b, clip);
    if (key) ui_hot(&c->u, b, clip, key);
    ui_line(&c->u, &t, label, 0, n, x + 2 + pad + ui_half(w - 4 - 2 * pad - tw), y + 2 + ui_half(h - 4 - t.L), light(c, p), clip);
    return w;
}

/* A row's text column: two blocks, the first over the second, wrapped at `w` and cut at the column's
   sides (.t's overflow-x: clip), so a word wider than it cannot run under the row's controls.
   Returns their height. Emits only with `emit`. */
static double text2(C* c, int r1, const char* s1, int r2, const char* s2, double x, double y, double w, int ink, const UIR* view, int emit)
{
    const UIR k = view ? ui_rect(x, view->y, w, view->h) : ui_rect(x, y, w, 0);     /* `view`, NULL: only measured */
    const double h1 = block(c, r1, c->l->g.line, s1, x, y, w, ink, &k, emit);
    return h1 + block(c, r2, c->l->g.line, s2, x, y + h1, w, ink, &k, emit);
}

/* ---- the list (#list): a row is the name over its detail, the badge and the action's box -------- */

static const int BADGE_PART[] = { -1, P_BADGE_ACTIVE, P_BADGE_UPDATE, P_BADGE_GREY, P_BADGE_CONFLICT };

static double badge_w(C* c, const MODSROW* r)
{
    return r->badge == MODS_BADGE_NONE ? 0 : 2 * U(c, c->l->g.badge_pad) + text_w(c, R_BADGE, r->badge_text);
}

/* the action's height, which the row's height must hold */
static double act_h(const C* c, const MODSROW* r)
{
    const MG* g = &c->l->g;
    switch (r->act) {
    case MODS_ACT_RADIO: case MODS_ACT_RADIO_ON: case MODS_ACT_CHECK: case MODS_ACT_CHECK_ON: return U(c, g->radio);
    case MODS_ACT_INSTALL: case MODS_ACT_UPDATE: return U(c, g->act.h);
    case MODS_ACT_BUSY: return r->progress >= 0 ? U(c, g->prog_h) : 0;
    default: return 0;
    }
}

/* The action at the right of its box, which ends at x1, and centred in the row's inner height `ih` from
   `top`: every row's badge stands in one column whichever action it has. A held radio shows on, the kit
   having no pressed face for it. */
static void action(C* c, const MODSROW* r, int i, double x1, double top, double ih, const UIR* clip)
{
    const MG* g = &c->l->g;
    const double h = act_h(c, r), y = top + ui_half(ih - h);
    char key[MODS_KEY];
    snprintf(key, sizeof key, "act:%d", i);
    switch (r->act) {
    case MODS_ACT_RADIO: case MODS_ACT_RADIO_ON: {
        const double d = U(c, g->radio);
        const UIR b = ui_rect(x1 - d, y, d, d);
        art(c, part(c, r->act == MODS_ACT_RADIO_ON || held(c, key) ? P_RADIO_ON : P_RADIO_OFF), b, clip);
        ui_hot(&c->u, b, clip, key);
        break;
    }
    case MODS_ACT_CHECK: case MODS_ACT_CHECK_ON: {
        const double d = U(c, g->radio);
        const UIR b = ui_rect(x1 - d, y, d, d);
        art(c, part(c, r->act == MODS_ACT_CHECK_ON || held(c, key) ? P_CHECK_ON : P_CHECK_OFF), b, clip);
        ui_hot(&c->u, b, clip, key);
        break;
    }
    case MODS_ACT_INSTALL: case MODS_ACT_UPDATE: {
        const char* s = r->act == MODS_ACT_INSTALL ? S_INSTALL : S_UPDATE;
        button(c, &g->act, x1 - ui_button_w(&c->u, &g->act, s), y, key, s, clip);
        break;
    }
    case MODS_ACT_BUSY:
        if (r->progress >= 0) {
            const double w = U(c, g->prog_w), in = U(c, g->prog_in);
            const UIR b = ui_rect(x1 - w, y, w, h);
            art(c, part(c, P_TRACK), b, clip);
            art(c, part(c, P_FILL), ui_rect(b.x + in, b.y + in, ui_lu((w - 2 * in) * r->progress / 1000), h - 2 * in), clip);
        }
        break;
    default:
        break;
    }
}

static double list_row(C* c, const UIPANEL* p, int i, double y0, double w, int emit)
{
    const MG* g = &c->l->g;
    const MODSROW* r = &c->st.row[i];
    const double py = U(c, g->row_py), px = U(c, g->row_px), gap = U(c, g->row_gap), aw = U(c, g->act_w), bw = badge_w(c, r);
    const double fixed = aw + gap + (r->badge != MODS_BADGE_NONE ? bw + gap : 0), tw = fmax(0, w - 2 * px - fixed);
    const double y = p->cy + y0, bh = r->badge != MODS_BADGE_NONE ? U(c, g->badge_h) : 0;
    const double th = text2(c, R_NAME, r->name, R_DETAIL, r->detail, 0, 0, tw, 0, NULL, 0);
    const double h = fmax(U(c, g->list_h), 2 * py + fmax(th, fmax(bh, act_h(c, r)))), ih = h - 2 * py;
    const UIR b = ui_rect(p->cx, y, w, h);
    char key[MODS_KEY];
    if (fixed > w - 2 * px) c->cut = 1;
    snprintf(key, sizeof key, "row:%d", i);
    if (emit && shows(b, &p->view)) {
        const int rp = i == c->st.selected || held(c, key) ? P_ROW_SEL : P_ROW, ink = light(c, rp);
        const double tx = p->cx + px;
        art(c, part(c, rp), b, &p->view);
        ui_hot(&c->u, b, &p->view, key);
        text2(c, R_NAME, r->name, R_DETAIL, r->detail, tx, y + py + ui_half(ih - th), tw, ink, &p->view, 1);
        if (r->badge != MODS_BADGE_NONE) {
            const int bp = BADGE_PART[r->badge];
            const double bx = tx + tw + gap, by = y + py + ui_half(ih - bh);
            UITS t = ts(c, R_BADGE);
            art(c, part(c, bp), ui_rect(bx, by, bw, bh), &p->view);
            line_at(c, R_BADGE, r->badge_text, bx + U(c, g->badge_pad), by + ui_half(bh - t.L), light(c, bp), &p->view);
        }
        action(c, r, i, p->cx + w - px, y + py, ih, &p->view);
    }
    return h + U(c, g->row_after);
}

/* `empty` centred in the scroller's content box: each line in the box's width, the lines in its height */
static double list_empty(C* c, const UIPANEL* p, double w, int emit)
{
    UITS t = ts(c, R_EMPTY);
    const char* s = c->st.empty;
    const int n = ui_pens(&c->u, &t, s, strlen(s)), ink = light(c, P_PANEL);
    int nl, i;
    double th, top;
    if (!n) return 0;
    nl = ui_wrap(&c->u, n, w);
    th = nl * t.L;
    if (!emit) return th;
    top = p->cy + ui_half(p->scr.h - 2 * U(c, c->l->g.pnl.pad[0]) - th);
    for (i = 0; i < nl; i++) {
        const double lw = ui_ceil64(c->u.pen[c->u.le[i]] - c->u.pen[c->u.ls[i]]);
        ui_line(&c->u, &t, s, c->u.ls[i], c->u.le[i], p->cx + ui_half(w - lw), top + i * t.L, ink, &p->view);
    }
    return th;
}

static double list_rows(UI* u, const UIPANEL* p, double w, int emit, void* user)
{
    C* c = (C*)user;
    double y = 0;
    int i;
    (void)u;
    if (!c->st.nrow) return list_empty(c, p, w, emit);
    for (i = 0; i < c->st.nrow; i++) y += list_row(c, p, i, y, w, emit);
    return y;
}

/* ---- the pane's rows (.opt): the text and the value ------------------------------------------- */

/* .v: an option's value, which the player reads and does not set (the mod decides it), its text centred in its box */
static double value_w(C* c, const char* v) { return fmax(U(c, c->l->g.value), text_w(c, R_VALUE, v)); }

/* Option i, from `y0` in the list. While locked it has a lock disc before its text and its reason under it, and all but
   the disc is one group faded once. Returns its height with the gap after it. */
static double opt_row(C* c, const UIPANEL* p, int i, double y0, double w, int emit)
{
    const MG* g = &c->l->g;
    const MODSOPT* o = &c->st.opt[i];
    const int locked = o->locked;
    const char* reason = locked ? o->reason : "";
    const UITS vt = ts(c, R_VALUE);
    const double py = U(c, g->row_py), px = U(c, g->row_px), gap = U(c, g->row_gap), lock = locked ? U(c, g->lock) : 0;
    const double cw = o->value[0] ? value_w(c, o->value) : 0, ch = o->value[0] ? vt.L : 0;
    const double fixed = (locked ? lock + gap : 0) + cw, tw = fmax(0, w - 2 * px - fixed - gap);
    const double th = text2(c, R_OPTION, o->label, R_REASON, reason, 0, 0, tw, 0, NULL, 0);
    const double h = fmax(U(c, g->row_h), 2 * py + fmax(fmax(th, ch), lock)), ih = h - 2 * py;
    if (fixed + gap > w - 2 * px) c->cut = 1;
    if (emit && shows(ui_rect(p->cx, p->cy + y0, w, h), &p->view)) {
        const double y = p->cy + y0, x0 = p->cx + px, tx = x0 + (locked ? lock + gap : 0), x = tx + tw + gap;
        const int ink = light(c, P_ROW);
        if (locked) ui_group_begin(&c->u, OFF_A);
        art(c, part(c, P_ROW), ui_rect(p->cx, y, w, h), &p->view);
        text2(c, R_OPTION, o->label, R_REASON, reason, tx, y + py + ui_half(ih - th), tw, ink, &p->view, 1);
        if (o->value[0])
            line_at(c, R_VALUE, o->value, x + ui_half(cw - text_w(c, R_VALUE, o->value)), y + py + ui_half(ih - ch) + ui_half(ch - vt.L), ink, &p->view);
        if (locked) {
            const UIR d = ui_rect(x0, y + py + ui_half(ih - lock), lock, lock);
            ui_group_end(&c->u);
            if (shows(d, &p->view)) ui_disc(&c->u, d, &p->view);
        }
    }
    return h + U(c, g->row_after);
}

/* .hdr: its space above and below it */
static double header(C* c, const UIPANEL* p, const char* s, double y, double w, int emit)
{
    const MG* g = &c->l->g;
    const double above = U(c, g->head);
    return above + block(c, R_HEADER, g->line, s, p->cx, p->cy + y + above, w, light(c, P_PANEL), &p->view, emit) + U(c, g->head_after);
}

/* ---- the showcase (.logo, .hero, .rule, .banner, the blocks: mods.js showcase) ------------------- */

/* THE FLOW of the showcase's boxes as the browser stacks them: the bottom margin of one box and the top margin of the next
   collapse into the larger (CSS 2, 8.3.1; every margin here is positive). `y` is where the last box ended, `mb` its margin. */
typedef struct { double y, mb; } FLOW;
static double flow_top(FLOW* f, double mt) { f->y += fmax(f->mb, mt); f->mb = 0; return f->y; }
static void flow_box(FLOW* f, double h, double mb) { f->y += h; f->mb = mb; }

static UIR meet(UIR a, UIR b)
{
    const double x0 = fmax(a.x, b.x), y0 = fmax(a.y, b.y), x1 = fmin(a.x + a.w, b.x + b.w), y1 = fmin(a.y + a.h, b.y + b.h);
    return ui_rect(x0, y0, fmax(0, x1 - x0), fmax(0, y1 - y0));
}

/* The kit's picture `kind`_<id> of the mod shown, with its size in texels: -1 when the kit has none */
static int show_part(const C* c, const char* kind, int* pw, int* ph)
{
    char n[48];
    const HUDPART* k;
    int i;
    snprintf(n, sizeof n, "%s_%s", kind, c->st.show.id);
    if ((i = ui_find_part(c->l->kit, n)) < 0 || !(k = tagpu_hudlay_part(c->l->kit, i)) || k->pw <= 0 || k->ph <= 0) return -1;
    *pw = k->pw;
    *ph = k->ph;
    return i;
}

/* A paragraph `pre` that ends in the link `link` (either ""), `w` wide from (x, y), wrapped as one text, each line from x or
   centred in w (.c). The link's words are its own runs in LINK_RGB, and `key` the control over the box an inline <a> takes:
   from its first line's glyphs' top to its last's bottom, across every line of it. Returns the height. Emits only with `emit`. */
static double show_text(C* c, int role, const char* pre, const char* link, const char* key, double x, double y, double w, int centre,
                        int emit, const UIR* clip)
{
    UITS t = ts(c, role);
    const UIFACE* fc = &c->u.f->face[t.style];
    const double A = floorf(fc->ascent * (float)t.fs + 0.5f), D = floorf(fc->descent * (float)t.fs + 0.5f);
    const int np = (int)strlen(pre);
    double hx0 = 1e30, hx1 = -1e30, hy0 = 0, hy1 = 0;
    int n, k, nl, i;
    snprintf(c->join, sizeof c->join, "%s%s", pre, link);
    if (!(n = ui_pens(&c->u, &t, c->join, strlen(c->join)))) return 0;
    for (k = 0; k < n && c->u.off[k] < np; k++) {}
    nl = ui_wrap(&c->u, n, w);
    for (i = 0; emit && i < nl; i++) {
        const int a = c->u.ls[i], b = c->u.le[i];
        const double lx = x + (centre ? ui_half(w - ui_ceil64(c->u.pen[b] - c->u.pen[a])) : 0), top = y + i * t.L;
        const int ink = light(c, P_PANEL);
        int e = b < k ? b : k;
        /* the space before the link is the paragraph's, and draws nothing */
        while (e > a && c->u.cp[e - 1] == ' ') e--;
        if (e > a) ui_line(&c->u, &t, c->join, a, e, lx, top, ink, clip);
        if (b > k) {
            const int la = a > k ? a : k, before = c->u.o->n;
            const double x0 = lx + c->u.pen[la] - c->u.pen[a], base = top + A + floor((t.L - A - D) / 2);
            ui_line(&c->u, &t, c->join, la, b, x0, top, ink, clip);
            if (c->u.o->n > before) c->u.o->run[c->u.o->op[c->u.o->n - 1].run].rgb = LINK_RGB;
            if (hx1 < hx0) hy0 = base - A;
            hx0 = fmin(hx0, x0);
            hx1 = fmax(hx1, x0 + c->u.pen[b] - c->u.pen[la]);
            hy1 = base + D;
        }
    }
    if (emit && key && hx1 > hx0) ui_hot(&c->u, ui_rect(hx0, hy0, hx1 - hx0, hy1 - hy0), clip, key);
    return nl * t.L;
}

/* The head: the logo, or the name as the pane's title when the kit has none, then the version and the authors, then the
   website as link:0. Centred, or over a backdrop (`hero`) from the left at the logo's narrower share. */
static void show_head(C* c, const UIPANEL* p, FLOW* f, double x, double w, int hero, int emit)
{
    const MG* g = &c->l->g;
    const MODSSHOW* s = &c->st.show;
    const double para = U(c, g->para);
    char line[sizeof s->version + sizeof s->by + 8];
    const char* host = !strncmp(s->site, "https://", 8) ? s->site + 8 : s->site;
    char h[sizeof s->site];
    int pw, ph, logo = show_part(c, "logo", &pw, &ph);
    double top;
    if (logo >= 0) {
        const double a = (double)pw / ph, pt = hero ? 0 : U(c, g->logo_pad[0]), pb = U(c, hero ? g->hero_logo_after : g->logo_pad[1]);
        const double bw = fmin(ui_lu(w * (hero ? g->logo_hero_w : g->logo_w) / 100), ui_lu(U(c, g->logo_h) * a)), bh = ui_lu(bw / a);
        top = flow_top(f, 0);
        if (emit) art(c, logo, ui_rect(hero ? x : x + ui_half(w - bw), top + pt, bw, bh), &p->view);
        flow_box(f, pt + bh + pb, 0);
    } else {
        top = flow_top(f, 0);
        flow_box(f, block(c, R_PANE_TITLE, g->title_line, s->name, x, top, w, light(c, P_PANEL), &p->view, emit), para);
    }
    snprintf(line, sizeof line, "%s \xC2\xB7 %s", s->version, s->by);
    top = flow_top(f, 0);
    flow_box(f, show_text(c, R_META, line, "", NULL, x, top, w, !hero, emit, &p->view), para);
    snprintf(h, sizeof h, "%s", host);
    if (strchr(h, '/')) *strchr(h, '/') = 0;
    top = flow_top(f, 0);
    flow_box(f, show_text(c, R_META, "", h, "link:0", x, top, w, !hero, emit, &p->view), para);
}

/* A list: blocks [i0, i0 + n), filled down `cols` columns of as many rows as the first needs (a grid of minmax(0, 1fr)
   columns flowing by column): each row as tall as its tallest item, each item the kit's dot centred on its first line and
   its text inset past it. Returns its height. */
static double show_list(C* c, const UIPANEL* p, int i0, int n, double x, double y, double w, int emit)
{
    const MG* g = &c->l->g;
    const MODSSHOW* s = &c->st.show;
    const int cols = s->block[i0].cols, rows = (n + cols - 1) / cols, ink = light(c, P_PANEL);
    const double gap = U(c, g->list_gap), after = U(c, g->list_after), ind = U(c, g->list_indent), b = U(c, g->list_bullet);
    const double colw = ui_lu((w - gap * (cols - 1)) / cols), tw = fmax(0, colw - ind), L = ts(c, R_SUMMARY).L;
    double top = y;
    int r, k;
    for (r = 0; r < rows; r++) {
        double rh = 0;
        for (k = 0; k < cols; k++)
            if (k * rows + r < n) rh = fmax(rh, block(c, R_SUMMARY, g->line, s->text + s->block[i0 + k * rows + r].text, 0, 0, tw, 0, NULL, 0));
        for (k = 0; emit && k < cols; k++) {
            const double cx = x + k * (colw + gap);
            if (k * rows + r >= n) continue;
            art(c, part(c, P_DOT), ui_rect(cx + ui_lu((ind - b) / 2), top + ui_lu((L - b) / 2), b, b), &p->view);
            block(c, R_SUMMARY, g->line, s->text + s->block[i0 + k * rows + r].text, cx + ind, top, tw, ink, &p->view, 1);
        }
        top += rh + (r + 1 < rows ? after : 0);
    }
    return top - y;
}

/* The showcase: the head (over the backdrop when the kit has one), the rule, the banner, the blocks. Returns where the flow
   stands, its last margin pending, for what follows it in the pane. */
static FLOW show_rows(C* c, const UIPANEL* p, double w, int emit)
{
    const MG* g = &c->l->g;
    const MODSSHOW* s = &c->st.show;
    const double para = U(c, g->para), cx = p->cx;
    FLOW f = { p->cy, 0 };
    int kw, kh, bw, bh, i, n, nlink = 0;
    const int back = show_part(c, "backdrop", &kw, &kh), ban = show_part(c, "banner", &bw, &bh);
    double top;
    if (back >= 0) {
        /* the head's box is as tall as what it holds, so that is measured first; the backdrop covers it, its right edge on
           the box's and its middle on the box's middle (background-size: cover; mods.js fitBackdrops) */
        const double pt = U(c, g->hero_pad[0]), px = U(c, g->hero_pad[1]), pb = U(c, g->hero_pad[2]), a = (double)kw / kh;
        FLOW in;
        double ht, sh;
        top = flow_top(&f, 0);
        in.y = top + pt; in.mb = 0;
        show_head(c, p, &in, cx + px, w - 2 * px, 1, 0);
        ht = in.y + in.mb - top + pb;
        sh = fmax(w / a, ht);
        if (emit) {
            const UIR k = meet(ui_rect(cx, top, w, ht), p->view);
            art(c, back, ui_rect(cx + ui_lu(w - sh * a), top + ui_lu((ht - sh) / 2), ui_lu(sh * a), ui_lu(sh)), &k);
            in.y = top + pt; in.mb = 0;
            show_head(c, p, &in, cx + px, w - 2 * px, 1, 1);
        }
        flow_box(&f, ht, 0);
    } else {
        show_head(c, p, &f, cx, w, 0, emit);
    }
    top = flow_top(&f, U(c, g->rule[0]));
    if (emit && shows(ui_rect(cx, top, w, 1), &p->view)) ui_solid(&c->u, HOP_SOLID, ui_rect(cx, top, w, 1), RULE_RGB, RULE_A, &p->view);
    flow_box(&f, 1, U(c, g->rule[1]));
    if (ban >= 0) {
        const double h = ui_lu(w * bh / bw);
        top = flow_top(&f, 0);
        if (emit) art(c, ban, ui_rect(cx, top, w, h), &p->view);
        flow_box(&f, h, U(c, g->banner_after));
    }
    for (i = 0; i < s->nblock; i += n) {
        const MODSBLOCK* b = &s->block[i];
        const char* tx = s->text + b->text;
        n = 1;
        if (b->kind == MODS_BLOCK_ITEM) {
            while (i + n < s->nblock && s->block[i + n].kind == MODS_BLOCK_ITEM && !s->block[i + n].first) n++;
            top = flow_top(&f, 0);
            flow_box(&f, show_list(c, p, i, n, cx, top, w, emit), para);
        } else if (b->kind == MODS_BLOCK_LINK) {
            char key[MODS_KEY];
            const char* url = s->text + b->link;
            snprintf(key, sizeof key, "link:%d", ++nlink);
            top = flow_top(&f, 0);
            flow_box(&f, show_text(c, R_SUMMARY, tx, !strncmp(url, "https://", 8) ? url + 8 : url, key, cx, top, w, 0, emit, &p->view), para);
        } else {
            /* a section opens with one blank line: 1lh, the line's own height, collapsing with the paragraph's margin before */
            top = flow_top(&f, b->kind == MODS_BLOCK_SECTION ? ts(c, R_SUMMARY).L : 0);
            flow_box(&f, show_text(c, R_SUMMARY, tx, "", NULL, cx, top, w, 0, emit, &p->view), para);
        }
    }
    return f;
}

/* ---- the pane (#detail): the showcase, or the title, the meta and the summary; the note; the options -- */

static double detail_rows(UI* u, const UIPANEL* p, double w, int emit, void* user)
{
    C* c = (C*)user;
    const MODSPAGE* st = &c->st;
    const MG* g = &c->l->g;
    const int ink = light(c, P_PANEL);
    const double para = U(c, g->para);
    double y = 0;
    int i;
    (void)u;
    if (st->show.id[0]) {
        const FLOW f = show_rows(c, p, w, emit);
        y = f.y + f.mb - p->cy;
    }
    if (!st->show.id[0] && st->title[0]) y += block(c, R_PANE_TITLE, g->title_line, st->title, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (!st->show.id[0] && st->meta[0]) y += block(c, R_META, g->line, st->meta, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (!st->show.id[0] && st->summary[0]) y += block(c, R_SUMMARY, g->line, st->summary, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (st->note[0]) y += block(c, R_NOTE, g->line, st->note, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (st->nopt) y += header(c, p, S_OPTIONS, y, w, emit);
    for (i = 0; i < st->nopt; i++) y += opt_row(c, p, i, y, w, emit);
    return y;
}

/* A panel at `box` with its list: its scroll is the page's, and what it answers the frame's */
static void panel(C* c, int which, UIR box, UILIST list, const char* key)
{
    UISCROLL a;
    ui_panel(&c->u, &c->l->g.pnl, part(c, P_PANEL), box, c->st.scroll[which], list, c, key, &a);
    c->fr->scroll[which] = a.scroll;
    c->fr->scroll_max[which] = a.scroll_max;
    c->fr->track[which] = a.track;
    c->fr->thumb_y[which] = a.thumb_y;
    c->fr->thumb_h[which] = a.thumb_h;
}

/* ---- the window (#win) --------------------------------------------------------------------------- */

static void tabs(C* c, double x, double y)
{
    const MG* g = &c->l->g;
    int i;
    for (i = 0; i < MODS_NTAB; i++) {
        char key[MODS_KEY], label[32];
        const char* s = TAB_LABEL[i];
        snprintf(key, sizeof key, "tab:%d", i);
        if (i == MODS_TAB_UPDATES && c->st.updates > 0) {
            snprintf(label, sizeof label, "%s (%d)", s, c->st.updates);
            s = label;
        }
        x += ui_button(&c->u, &g->tab, x, y, key, s, -1, part(c, held(c, key) ? P_TAB_DOWN : i == c->st.tab ? P_TAB_SEL : P_TAB), 0) + U(c, g->tab_gap);
    }
}

/* Back at the left; at the right Confirm, greyed and no control with no label given, Update all before it */
static void footer(C* c, UIR body, double y)
{
    const MG* g = &c->l->g;
    const int off = !c->st.confirm[0];
    const char* cs = off ? S_CONFIRM : c->st.confirm;
    const double cw = ui_button_w(&c->u, &g->foot, cs), cx = body.x + body.w - cw;
    ui_button(&c->u, &g->foot, body.x, y, "back", S_BACK, -1, part(c, held(c, "back") ? P_BUTTON_DOWN : P_BUTTON), 0);
    if (c->st.update_all)
        ui_button(&c->u, &g->foot, cx - U(c, g->foot_gap) - ui_button_w(&c->u, &g->foot, S_UPDATEALL), y, "updateall", S_UPDATEALL, -1,
                  part(c, held(c, "updateall") ? P_BUTTON_DOWN : P_BUTTON), 0);
    ui_button(&c->u, &g->foot, cx, y, "confirm", cs, -1, part(c, !off && held(c, "confirm") ? P_BUTTON_DOWN : P_BUTTON), off);
}

static void window_(C* c)
{
    const MG* g = &c->l->g;
    const double W = c->st.W, H = c->st.H, gap = U(c, g->gap), th = U(c, g->tab.h), fh = U(c, g->foot.h);
    const UIR win = ui_rect(ui_lu((W - W * g->share / 100) / 2), ui_lu((H - H * g->share / 100) / 2), ui_lu(W * g->share / 100), ui_lu(H * g->share / 100));
    const UIR wt = ui_rect(win.x + U(c, g->wt[0]), win.y + U(c, g->wt[2]), win.w - U(c, g->wt[0]) - U(c, g->wt[1]), U(c, g->wt[3]));
    const UIR body = ui_rect(win.x + U(c, g->wb[3]), win.y + U(c, g->wb[0]), win.w - U(c, g->wb[3]) - U(c, g->wb[1]), win.h - U(c, g->wb[0]) - U(c, g->wb[2]));
    const double t0 = ui_lu(body.w * g->col / 100), t1 = fmax(0, body.w - t0 - gap), cy = body.y + th + gap, ch = fmax(0, body.h - th - fh - 2 * gap);
    UITS t = ts(c, R_WINDOW_TITLE);
    ui_solid(&c->u, HOP_SOLID, ui_rect(0, 0, W, H), BACK_RGB, 1, NULL);
    c->fr->window.x = (float)(win.x / c->u.px); c->fr->window.y = (float)(win.y / c->u.px);
    c->fr->window.w = (float)(win.w / c->u.px); c->fr->window.h = (float)(win.h / c->u.px);
    ui_art(&c->u, part(c, P_WINDOW), win, NULL);
    /* the title in its groove: its own ink, the style's whatever the window's face */
    line_at(c, R_WINDOW_TITLE, S_TITLE, wt.x + U(c, g->title_pad), wt.y + ui_half(wt.h - t.L), 0, &wt);
    {
        /* the close button in the window part's bay */
        const UIR wx = ui_rect(win.x + win.w - U(c, g->wx[0]) - U(c, g->wx[2]), win.y + U(c, g->wx[1]), U(c, g->wx[2]), U(c, g->wx[3]));
        ui_art(&c->u, part(c, !strcmp(c->st.held, "close") ? P_CLOSE_DOWN : P_CLOSE), wx, NULL);
        ui_hot(&c->u, wx, NULL, "close");
        ui_glyph(&c->u, part(c, P_ICON_CLOSE), wx);
    }
    tabs(c, body.x, body.y);
    panel(c, MODS_SCROLL_LIST, ui_rect(body.x, cy, t0, ch), list_rows, "scroll:list");
    panel(c, MODS_SCROLL_DETAIL, ui_rect(body.x + t0 + gap, cy, t1, ch), detail_rows, "scroll:detail");
    footer(c, body, cy + ch + gap);
}

/* ---- the entry points ---------------------------------------------------------------------------- */

/* MODS_MAXHOT, the bound: an option of the pane has no hit, so every hit is the list's, a showcase
   link's or the window's. The list holds MODS_MAXROW rows of 2 hits, a row and its action, 32; a
   showcase its website and at most one link a block, 1 + MODS_MAXBLOCK, 65; the tabs, the close
   button, the footer's three buttons and the two scrollers are 9. 106, under MODS_MAXHOT, so no
   control on screen goes without its hit; tagpu_modslay_check builds that page at every scroll. */
static int build(const MODSLAY* l, const UIFACES* f, const MODSPAGE* st, HUDOPS* out, MODSFRAME* fr, int* cut, char* err, size_t errn)
{
    C* c;
    if (err && errn) err[0] = 0;
    if (!l || !f || !st || !out || !fr || !(st->H > 0) || !(st->W > 0)) {
        if (err && errn) snprintf(err, errn, "modslay: no layout, faces, page or screen");
        return 0;
    }
    /* the context holds a text's pens and the page: about 40 KB, off the render thread's stack */
    if (!(c = (C*)malloc(sizeof *c))) {
        if (err && errn) snprintf(err, errn, "modslay: out of memory");
        return 0;
    }
    c->st = *st;
    page_bound(&c->st);
    ui_begin(&c->u, out, f, l->style, l->light, (double)st->H / 720, MODS_MAXHOT);
    c->l = l; c->fr = fr; c->cut = 0;
    memset(fr, 0, sizeof *fr);
    out->n = out->nrun = out->ntext = out->overflow = out->ngrp = 0;
    out->first = out->last = 0;
    out->track_x0 = out->track_x1 = 0;
    window_(c);
    if (cut) *cut = c->cut;
    free(c);
    return 1;
}

int tagpu_modslay_build(const MODSLAY* l, const UIFACES* f, const MODSPAGE* st, HUDOPS* out, MODSFRAME* fr, char* err, size_t errn)
{
    return build(l, f, st, out, fr, NULL, err, errn);
}

/* `n` - 1 of the widest letter, and with `broken` a space at every other place, which breaks into the
   most lines */
static void worst(char* d, size_t n, int broken)
{
    size_t i;
    for (i = 0; i + 1 < n; i++) d[i] = broken && (i & 1) ? ' ' : 'W';
    d[n - 1] = 0;
    if (broken && n >= 2 && d[n - 2] == ' ') d[n - 2] = 'W';
}
#define WORST(a, b) worst((a), sizeof(a), (b))

/* The fullest showcase: every block, the text full, in runs of four items of the most columns, a paragraph, a section and
   two links, or with `links` every block a link, the most hits (MODS_MAXHOT). Its id names no picture, which adds three
   parts at most and no text. */
static void fullest_show(MODSSHOW* s, int broken, int links)
{
    const int per = MODS_SHOWTEXT / MODS_MAXBLOCK;
    int i;
    snprintf(s->id, sizeof s->id, "fullest");
    WORST(s->name, broken); WORST(s->version, broken); WORST(s->by, broken);
    worst(s->site + 8, sizeof s->site - 8, broken);
    memcpy(s->site, "https://", 8);
    s->nblock = MODS_MAXBLOCK;
    for (i = 0; i < MODS_MAXBLOCK; i++) {
        MODSBLOCK* b = &s->block[i];
        const int r = i % 8;
        b->kind = (uint8_t)(links ? MODS_BLOCK_LINK : r < 4 ? MODS_BLOCK_ITEM : r == 4 ? MODS_BLOCK_PARA : r == 5 ? MODS_BLOCK_SECTION : MODS_BLOCK_LINK);
        b->cols = MODS_MAXCOLS;
        b->first = (uint8_t)(r == 0);
        b->text = (uint16_t)(i * per);
        if (b->kind == MODS_BLOCK_LINK) {
            b->link = (uint16_t)(i * per + per / 2);
            worst(s->text + b->text, (size_t)per / 2, broken);
            worst(s->text + b->link, (size_t)(per - per / 2), broken);
            memcpy(s->text + b->link, "https://", 8);
        } else {
            worst(s->text + b->text, (size_t)per, broken);
        }
    }
}

/* the fullest page there can be: every array full, every text at its array's length. `locked` greys
   every option: the most groups. */
static void fullest(MODSPAGE* s, float W, float H, int broken, int locked)
{
    int i;
    memset(s, 0, sizeof *s);
    s->W = W; s->H = H;
    s->tab = MODS_TAB_UPDATES;
    s->updates = 999;
    s->nrow = MODS_MAXROW;
    s->selected = 0;
    s->nopt = MODS_MAXOPT;
    s->update_all = 1;
    WORST(s->title, broken); WORST(s->meta, broken); WORST(s->summary, broken); WORST(s->note, broken); WORST(s->confirm, broken);
    for (i = 0; i < MODS_MAXROW; i++) {
        MODSROW* r = &s->row[i];
        WORST(r->name, broken); WORST(r->detail, broken); WORST(r->badge_text, broken);
        r->badge = (uint8_t)(1 + i % MODS_BADGE_CONFLICT);
        r->act = (uint8_t)(1 + i % MODS_ACT_CHECK_ON);
        r->progress = 500;
    }
    for (i = 0; i < MODS_MAXOPT; i++) {
        MODSOPT* o = &s->opt[i];
        WORST(o->label, broken); WORST(o->value, broken); WORST(o->reason, broken);
        o->locked = (uint8_t)locked;
    }
    /* broken texts make the most lines, so the showcase is checked with them, and the plain pane with the widest words; with
       every option locked, every block of it is a link */
    if (broken) fullest_show(&s->show, broken, locked);
}

int tagpu_modslay_check(const MODSLAY* l, const UIFACES* f, HUDOPS* scratch, char* err, size_t errn)
{
    static const float SCREEN[][2] = { { 1024, 768 }, { 1280, 720 }, { 3840, 2160 } };
    MODSPAGE* st;
    MODSFRAME fr;
    int s, broken, locked, ok = 1;
    if (err && errn) err[0] = 0;
    if (!l || !f || !scratch) {
        if (err && errn) snprintf(err, errn, "modslay: no layout, faces or scratch to check");
        return 0;
    }
    if (!(st = (MODSPAGE*)malloc(sizeof *st))) {
        if (err && errn) snprintf(err, errn, "modslay: out of memory");
        return 0;
    }
    for (s = 0; ok && s < (int)(sizeof SCREEN / sizeof *SCREEN); s++)
        for (broken = 0; ok && broken < 2; broken++)
            for (locked = 0; ok && locked < 2; locked++) {
                /* each panel at each scroll, a screenful of rows being what bounds the hits and the ops */
                float y = 0, top;
                fullest(st, SCREEN[s][0], SCREEN[s][1], broken, locked);
                do {
                    int cut = 0;
                    st->scroll[MODS_SCROLL_DETAIL] = st->scroll[MODS_SCROLL_LIST] = y;
                    if (!(ok = build(l, f, st, scratch, &fr, &cut, err, errn))) break;
                    if (scratch->overflow) {
                        if (err && errn)
                            snprintf(err, errn, "mods layout: the fullest page overflows its ops at %.0fx%.0f, scroll %.0f%s%s", st->W, st->H, y,
                                     broken ? ", broken texts" : "", locked ? ", all locked" : "");
                        ok = 0;
                    } else if (cut) {
                        if (err && errn)
                            snprintf(err, errn, "mods layout: a row's controls are wider than the row at %.0fx%.0f", st->W, st->H);
                        ok = 0;
                    }
                    y += 50;
                    top = fr.scroll_max[MODS_SCROLL_DETAIL] > fr.scroll_max[MODS_SCROLL_LIST] ? fr.scroll_max[MODS_SCROLL_DETAIL] : fr.scroll_max[MODS_SCROLL_LIST];
                } while (ok && y - 50 < top);
            }
    free(st);
    return ok;
}

/* ---- loading ------------------------------------------------------------------------------------- */

static int read_layout(MODSLAY* l, const TJV* L, char* err, size_t errn)
{
    UILAYOUT x;
    MG* g = &l->g;
    int i;
    x.L = L; x.bad = 0; x.err = err; x.errn = errn; x.what = "mods layout";
    g->share = ui_num(&x, "window", "share", -1);
    g->line = ui_num(&x, "window", "line", -1);
    g->tab.line = g->foot.line = g->act.line = g->line;
    for (i = 0; i < 4; i++) {
        g->wt[i] = ui_num(&x, "wt", NULL, i);
        g->wx[i] = ui_num(&x, "wx", NULL, i);
        g->wb[i] = ui_num(&x, "wb", NULL, i);
        g->pnl.pn[i] = ui_num(&x, "pn", NULL, i);
    }
    g->gap = ui_num(&x, "gap", NULL, -1);
    g->title_pad = ui_num(&x, "title", "pad", -1);
    g->tab.h = ui_num(&x, "tabs", "h", -1);
    g->tab.pad = ui_num(&x, "tabs", "pad", -1);
    g->tab.min = ui_num(&x, "tabs", "min", -1);
    g->tab_gap = ui_num(&x, "tabs", "gap", -1);
    g->col = ui_num(&x, "columns", NULL, 0);
    for (i = 0; i < 2; i++) g->pnl.pad[i] = ui_num(&x, "panel", "pad", i);
    g->pnl.scrollbar = ui_num(&x, "scrollbar", NULL, -1);
    g->list_h = ui_num(&x, "list", "h", -1);
    g->row_h = ui_num(&x, "row", "h", -1);
    g->row_py = ui_num(&x, "row", "pad", 0);
    g->row_px = ui_num(&x, "row", "pad", 1);
    g->row_gap = ui_num(&x, "row", "gap", -1);
    g->row_after = ui_num(&x, "row", "after", -1);
    g->lock = ui_num(&x, "lock", NULL, -1);
    g->radio = ui_num(&x, "radio", NULL, -1);
    g->act_w = ui_num(&x, "act", "w", -1);
    g->act.h = ui_num(&x, "act", "h", -1);
    g->act.pad = ui_num(&x, "act", "pad", -1);
    g->act.min = ui_num(&x, "act", "min", -1);
    g->value = ui_num(&x, "value", NULL, -1);
    g->badge_h = ui_num(&x, "badge", "h", -1);
    g->badge_pad = ui_num(&x, "badge", "pad", -1);
    g->prog_w = ui_num(&x, "progress", "w", -1);
    g->prog_h = ui_num(&x, "progress", "h", -1);
    g->prog_in = ui_num(&x, "progress", "inset", -1);
    g->title_line = ui_num(&x, "pane", "title_line", -1);
    g->para = ui_num(&x, "pane", "para", -1);
    g->head = ui_num(&x, "pane", "head", -1);
    g->head_after = ui_num(&x, "pane", "after", -1);
    g->logo_h = ui_num(&x, "show_logo", "h", -1);
    for (i = 0; i < 2; i++) g->logo_pad[i] = ui_num(&x, "show_logo", "pad", i);
    g->logo_w = ui_num(&x, "show_logo", "w", -1);
    g->logo_hero_w = ui_num(&x, "show_logo", "hero_w", -1);
    for (i = 0; i < 3; i++) g->hero_pad[i] = ui_num(&x, "show_hero", "pad", i);
    g->hero_logo_after = ui_num(&x, "show_hero", "logo_after", -1);
    for (i = 0; i < 2; i++) g->rule[i] = ui_num(&x, "show_rule", "pad", i);
    g->banner_after = ui_num(&x, "show_banner", "after", -1);
    g->list_indent = ui_num(&x, "show_list", "indent", -1);
    g->list_bullet = ui_num(&x, "show_list", "bullet", -1);
    g->list_gap = ui_num(&x, "show_list", "gap", -1);
    g->list_after = ui_num(&x, "show_list", "after", -1);
    g->foot.h = ui_num(&x, "footer", "h", -1);
    g->foot.pad = ui_num(&x, "footer", "pad", -1);
    g->foot.min = ui_num(&x, "footer", "min", -1);
    g->foot_gap = ui_num(&x, "footer", "gap", -1);
    return !x.bad;
}

MODSLAY* tagpu_modslay_load(const char* layout, size_t nlayout, const HUDLAY* kit, char* err, size_t errn)
{
    MODSLAY* l = (MODSLAY*)calloc(1, sizeof(MODSLAY));
    TJDOC* doc = NULL;
    const TJV* roles;
    const HUDGLOBALS* styles = kit ? tagpu_hudlay_globals(kit) : NULL;
    char e2[200];
    int i, j, ok = 0;
    if (err && errn) err[0] = 0;
    if (!l || !styles) { snprintf(err, errn, "modslay: out of memory or no kit"); goto done; }
    l->kit = kit;
    if (!(doc = tagpu_json_parse(layout, nlayout, e2, sizeof e2))) { snprintf(err, errn, "mods layout.json: %s", e2); goto done; }
    if (!read_layout(l, tagpu_json_root(doc), err, errn)) goto done;
    for (i = 0; i < P_N; i++)
        if ((l->part[i] = ui_find_part(kit, PART[i])) < 0) { snprintf(err, errn, "mods: the kit has no part '%s'", PART[i]); goto done; }
    l->nstyle = styles->nstyle;
    memcpy(l->style, styles->style, sizeof l->style);
    roles = tj_get(tagpu_json_root(doc), "roles");
    for (i = 0; i < R_N; i++) {
        const char* sn = tj_str(tj_get(roles, ROLE[i]), NULL);
        for (j = 0; sn && j < l->nstyle && strcmp(l->style[j].name, sn); j++) {}
        if (!sn || j >= l->nstyle) { snprintf(err, errn, "mods layout: roles.%s names no HUD style", ROLE[i]); goto done; }
        l->role[i] = j;
    }
    l->g.tab.style = l->role[R_TAB];
    l->g.foot.style = l->role[R_FOOTER];
    l->g.act.style = l->role[R_ACT];
    ok = 1;
done:
    tagpu_json_free(doc);
    if (!ok) { tagpu_modslay_free(l); return NULL; }
    return l;
}

void tagpu_modslay_free(MODSLAY* l) { free(l); }

void tagpu_modslay_light(MODSLAY* l, int part_, int lt)
{
    if (l && part_ >= 0 && part_ < UI_MAXPART) l->light[part_] = (unsigned char)(lt != 0);
}

/* ---- a catalog entry's showcase ------------------------------------------------------------------- */

typedef struct { MODSSHOW* s; size_t used; int ok; } SHOWPUT;

/* `str` into the text, NUL-terminated: its offset, 0 with `ok` cleared when it does not fit */
static uint16_t show_put(SHOWPUT* q, const char* str)
{
    const size_t n = strlen(str) + 1;
    const size_t at = q->used;
    if (!q->ok || at + n > MODS_SHOWTEXT) { q->ok = 0; return 0; }
    memcpy(q->s->text + at, str, n);
    q->used += n;
    return (uint16_t)at;
}
/* `src` into `dst` of `n` bytes, `ok` cleared when it would be cut */
static void show_copy(SHOWPUT* q, char* dst, size_t n, const char* src)
{
    if ((size_t)snprintf(dst, n, "%s", src) >= n) q->ok = 0;
}
static MODSBLOCK* show_block(SHOWPUT* q, int kind, const char* text)
{
    MODSBLOCK* b;
    if (!q->ok || q->s->nblock >= MODS_MAXBLOCK || strlen(text) > MODS_SHOWLINE) { q->ok = 0; return NULL; }
    b = &q->s->block[q->s->nblock++];
    b->kind = (uint8_t)kind;
    b->cols = 1;
    b->text = show_put(q, text);
    return q->ok ? b : NULL;
}

int tagpu_modslay_show(const TJV* show, const char* id, const char* name, const char* version, MODSSHOW* s)
{
    const TJV* about = tj_get(show, "about");
    const TJV *b, *v;
    SHOWPUT q;
    int i, j;
    memset(s, 0, sizeof *s);
    if (!id || !id[0] || !tj_is(about, TJ_ARR)) return 0;
    q.s = s; q.used = 0; q.ok = 1;
    show_copy(&q, s->id, sizeof s->id, id);
    show_copy(&q, s->name, sizeof s->name, name ? name : id);
    show_copy(&q, s->version, sizeof s->version, version ? version : "");
    show_copy(&q, s->by, sizeof s->by, tj_str(tj_get(show, "by"), ""));
    show_copy(&q, s->site, sizeof s->site, tj_str(tj_get(show, "site"), ""));
    if (strncmp(s->site, "https://", 8)) q.ok = 0;
    for (i = 0; q.ok && (b = tj_at(about, i)); i++) {
        if (tj_is(b, TJ_STR)) show_block(&q, MODS_BLOCK_PARA, tj_str(b, ""));
        else if ((v = tj_get(b, "section"))) show_block(&q, MODS_BLOCK_SECTION, tj_str(v, ""));
        else if ((v = tj_get(b, "link"))) {
            MODSBLOCK* k = show_block(&q, MODS_BLOCK_LINK, tj_str(tj_at(v, 0), ""));
            const char* url = tj_str(tj_at(v, 1), "");
            if (strncmp(url, "https://", 8) || (k && strlen(s->text + k->text) + strlen(url) > MODS_SHOWLINE)) q.ok = 0;
            else if (k) k->link = show_put(&q, url);
        } else if ((v = tj_get(b, "list")) && tj_is(v, TJ_ARR)) {
            const int cols = clampi((int)tj_num(tj_get(b, "cols"), 1), 1, MODS_MAXCOLS);
            const TJV* it;
            for (j = 0; q.ok && (it = tj_at(v, j)); j++) {
                MODSBLOCK* k = show_block(&q, MODS_BLOCK_ITEM, tj_str(it, ""));
                if (k) { k->cols = (uint8_t)cols; k->first = (uint8_t)(j == 0); }
            }
        } else q.ok = 0;
    }
    if (!q.ok) memset(s, 0, sizeof *s);
    return q.ok;
}
