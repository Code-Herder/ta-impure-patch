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
enum { P_WINDOW, P_PANEL, P_ROW, P_ROW_SEL, P_BUTTON, P_BUTTON_DOWN, P_TAB, P_TAB_SEL, P_TAB_DOWN, P_CHECK_OFF, P_CHECK_ON,
       P_RADIO_OFF, P_RADIO_ON, P_TRACK, P_FILL, P_BADGE_ACTIVE, P_BADGE_UPDATE, P_BADGE_GREY, P_BADGE_CONFLICT, P_N };
static const char* const PART[P_N] = { "window", "panel", "row", "row_selected", "button", "button_down", "tab", "tab_selected",
    "tab_down", "check_off", "check_on", "radio_off", "radio_on", "progress_track", "progress_fill", "badge_active",
    "badge_update", "badge_grey", "badge_conflict" };

/* layout.json `roles`, the text of each element */
enum { R_WINDOW_TITLE, R_TAB, R_NAME, R_DETAIL, R_ACT, R_BADGE, R_EMPTY, R_PANE_TITLE, R_META, R_SUMMARY, R_NOTE, R_HEADER,
       R_OPTION, R_REASON, R_VALUE, R_STEP, R_RESET, R_ARCHIVE, R_FOOTER, R_N };
static const char* const ROLE[R_N] = { "window_title", "tab", "name", "detail", "act", "badge", "empty", "pane_title", "meta",
    "summary", "note", "header", "option", "reason", "value", "step", "reset", "archive", "footer" };

/* The mockup's strings (mods.js) and the colours its CSS gives outright (mods.css). modslaycheck.py
   compares every text, dot and solid, so one that drifts from the mockup's is named there. */
#define S_TITLE     "Mods"
#define S_BACK      "Back"
#define S_CONFIRM   "Confirm"
#define S_UPDATEALL "Update all"
#define S_INSTALL   "Install"
#define S_UPDATE    "Update"
#define S_RESET     "Reset"
#define S_OPTIONS   "Play options"
#define S_ARCHIVES  "Game archives"
static const char* const TAB_LABEL[MODS_NTAB] = { "Manage", "Explore", "Updates" };
#define BACK_RGB    0x0B0C0Au       /* the backdrop: opaque, the engine's menu is under it */
#define MARK_RGB    0xE2A030u       /* the marker of a value the player changed            */
#define OFF_A       0.45            /* a greyed row: a disabled button's (tagpu_uikit.c)   */

/* layout.json's numbers, in units unless named */
typedef struct {
    double share, line;             /* the window's share (percent), the text's line height */
    double wt[4], wb[4], gap;       /* wt l r t h, wb t r b l */
    UIPANELG pnl;
    double title_pad;
    UIBUTTONG tab, foot, act, step, rst;
    double tab_gap, foot_gap, small_gap;
    double col;                     /* the list's share of the body, percent */
    double list_h;
    double row_h, row_py, row_px, row_gap, row_after;
    double lock, marker, check, radio, value, act_w;
    double badge_h, badge_pad;
    double prog_w, prog_h, prog_in;
    double title_line, para, head, head_after;
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
    s->nsw = clampi(s->nsw, 0, MODS_MAXSW);
    if (s->selected < 0 || s->selected >= s->nrow) s->selected = -1;
    for (i = 0; i < s->nrow; i++) {
        MODSROW* r = &s->row[i];
        TERM(r->name); TERM(r->detail); TERM(r->badge_text);
        if (r->badge > MODS_BADGE_CONFLICT) r->badge = MODS_BADGE_NONE;
        if (r->act > MODS_ACT_BUSY) r->act = MODS_ACT_NONE;
        r->progress = (int16_t)clampi(r->progress, -1, 1000);
    }
    for (i = 0; i < s->nopt; i++) {
        MODSOPT* o = &s->opt[i];
        TERM(o->label); TERM(o->value); TERM(o->reason);
        if (o->kind > MODS_OPT_STEPPER) o->kind = MODS_OPT_SWITCH;
    }
    for (i = 0; i < s->nsw; i++) TERM(s->sw[i].name);
    TERM(s->empty); TERM(s->title); TERM(s->meta); TERM(s->summary); TERM(s->note); TERM(s->held); TERM(s->confirm);
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
    case MODS_ACT_RADIO: case MODS_ACT_RADIO_ON: return U(c, g->radio);
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

/* ---- the pane's rows (.opt): the text, the marker and reset while changed, and the control ------- */

/* A row's control (.ctl), a flex row of the stepper's minus, value and plus, or of the value and the
   switch's box; an archive has no value. A key "" makes it no control: a locked row's. */
typedef struct {
    int         stepper, on;
    const char* value;
    const char* key;        /* the switch's, over the value and the box together, the player's target */
    const char* minus;
    const char* plus;
} CTL;

static double value_w(C* c, const CTL* k) { return fmax(U(c, c->l->g.value), text_w(c, R_VALUE, k->value)); }

static void ctl_size(C* c, const CTL* k, double* w, double* h)
{
    const MG* g = &c->l->g;
    const double gap = U(c, g->small_gap), v = value_w(c, k), L = k->value[0] ? ts(c, R_VALUE).L : 0;
    if (k->stepper) {
        *w = ui_button_w(&c->u, &g->step, "-") + gap + v + gap + ui_button_w(&c->u, &g->step, "+");
        *h = fmax(U(c, g->step.h), L);
    } else {
        *w = (k->value[0] ? v + gap : 0) + U(c, g->check);
        *h = fmax(U(c, g->check), L);
    }
}

/* .v: its text centred in the value's box */
static void value_at(C* c, const CTL* k, double x, double top, double ch, int ink, const UIR* clip)
{
    UITS t = ts(c, R_VALUE);
    if (k->value[0]) line_at(c, R_VALUE, k->value, x + ui_half(value_w(c, k) - text_w(c, R_VALUE, k->value)), top + ui_half(ch - t.L), ink, clip);
}

/* A held switch shows on, the kit having no pressed face for it. */
static void ctl_draw(C* c, const CTL* k, double x, double top, double cw, double ch, int ink, const UIR* clip)
{
    const MG* g = &c->l->g;
    const double gap = U(c, g->small_gap), v = value_w(c, k);
    if (k->stepper) {
        const double by = top + ui_half(ch - U(c, g->step.h));
        const double mw = button(c, &g->step, x, by, k->minus[0] ? k->minus : NULL, "-", clip);
        value_at(c, k, x + mw + gap, top, ch, ink, clip);
        button(c, &g->step, x + mw + gap + v + gap, by, k->plus[0] ? k->plus : NULL, "+", clip);
    } else {
        const double d = U(c, g->check);
        if (k->key[0]) ui_hot(&c->u, ui_rect(x, top, cw, ch), clip, k->key);
        value_at(c, k, x, top, ch, ink, clip);
        art(c, part(c, k->on || held(c, k->key) ? P_CHECK_ON : P_CHECK_OFF), ui_rect(x + cw - d, top + ui_half(ch - d), d, d), clip);
    }
}

/* One row of the pane, from `y0` in the list: an option's or an archive's. While `locked` it has a lock
   disc before its text and no reset, and all but the disc is one group faded once. Returns its height
   with the gap after it. */
static double pane_row(C* c, const UIPANEL* p, int role, const char* label, const char* reason, int locked, int changed,
                       const char* reset_key, const CTL* k, double y0, double w, int emit)
{
    const MG* g = &c->l->g;
    const int mark = changed && !locked;
    const double py = U(c, g->row_py), px = U(c, g->row_px), gap = U(c, g->row_gap), lock = locked ? U(c, g->lock) : 0;
    const double rw = mark ? ui_button_w(&c->u, &g->rst, S_RESET) : 0, md = mark ? U(c, g->marker) : 0, rh = mark ? U(c, g->rst.h) : 0;
    double cw, ch, fixed, tw, th, h, ih;
    ctl_size(c, k, &cw, &ch);
    fixed = (locked ? lock + gap : 0) + (mark ? md + gap + rw + gap : 0) + cw;
    if (fixed + gap > w - 2 * px) c->cut = 1;
    tw = fmax(0, w - 2 * px - fixed - gap);
    th = text2(c, role, label, R_REASON, reason, 0, 0, tw, 0, NULL, 0);
    h = fmax(U(c, g->row_h), 2 * py + fmax(fmax(fmax(th, ch), lock), fmax(rh, md)));
    ih = h - 2 * py;
    if (emit && shows(ui_rect(p->cx, p->cy + y0, w, h), &p->view)) {
        const double y = p->cy + y0, x0 = p->cx + px, tx = x0 + (locked ? lock + gap : 0);
        const int ink = light(c, P_ROW);
        double x = tx + tw + gap;
        if (locked) ui_group_begin(&c->u, OFF_A);
        art(c, part(c, P_ROW), ui_rect(p->cx, y, w, h), &p->view);
        text2(c, role, label, R_REASON, reason, tx, y + py + ui_half(ih - th), tw, ink, &p->view, 1);
        if (mark) {
            const UIR m = ui_rect(x, y + py + ui_half(ih - md), md, md);
            if (shows(m, &p->view)) ui_solid(&c->u, HOP_DOT, m, MARK_RGB, 1, &p->view);
            x += md + gap;
            x += button(c, &g->rst, x, y + py + ui_half(ih - rh), reset_key, S_RESET, &p->view) + gap;
        }
        ctl_draw(c, k, x, y + py + ui_half(ih - ch), cw, ch, ink, &p->view);
        if (locked) {
            const UIR d = ui_rect(x0, y + py + ui_half(ih - lock), lock, lock);
            ui_group_end(&c->u);
            if (shows(d, &p->view)) ui_disc(&c->u, d, &p->view);
        }
    }
    return h + U(c, g->row_after);
}

static double opt_row(C* c, const UIPANEL* p, int i, double y, double w, int emit)
{
    const MODSOPT* o = &c->st.opt[i];
    char t[MODS_KEY] = "", m[MODS_KEY] = "", pl[MODS_KEY] = "", r[MODS_KEY];
    CTL k;
    if (!o->locked) {
        snprintf(t, sizeof t, "opt:%d:t", i);
        snprintf(m, sizeof m, "opt:%d:-", i);
        snprintf(pl, sizeof pl, "opt:%d:+", i);
    }
    snprintf(r, sizeof r, "reset:%d", i);
    k.stepper = o->kind == MODS_OPT_STEPPER;
    k.on = o->on;
    k.value = o->value;
    k.key = t; k.minus = m; k.plus = pl;
    return pane_row(c, p, R_OPTION, o->label, o->locked ? o->reason : "", o->locked, o->changed, r, &k, y, w, emit);
}

static double sw_row(C* c, const UIPANEL* p, int i, double y, double w, int emit)
{
    const MODSSWITCH* s = &c->st.sw[i];
    char t[MODS_KEY] = "", r[MODS_KEY];
    CTL k;
    if (!s->locked) snprintf(t, sizeof t, "sw:%d", i);
    snprintf(r, sizeof r, "swreset:%d", i);
    k.stepper = 0;
    k.on = s->on;
    k.value = "";
    k.key = t; k.minus = ""; k.plus = "";
    return pane_row(c, p, R_ARCHIVE, s->name, "", s->locked, s->changed, r, &k, y, w, emit);
}

/* .hdr: its space above and below it */
static double header(C* c, const UIPANEL* p, const char* s, double y, double w, int emit)
{
    const MG* g = &c->l->g;
    const double above = U(c, g->head);
    return above + block(c, R_HEADER, g->line, s, p->cx, p->cy + y + above, w, light(c, P_PANEL), &p->view, emit) + U(c, g->head_after);
}

/* ---- the pane (#detail): the title, the meta, the summary, the note, and the two lists ----------- */

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
    if (st->title[0]) y += block(c, R_PANE_TITLE, g->title_line, st->title, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (st->meta[0]) y += block(c, R_META, g->line, st->meta, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (st->summary[0]) y += block(c, R_SUMMARY, g->line, st->summary, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (st->note[0]) y += block(c, R_NOTE, g->line, st->note, p->cx, p->cy + y, w, ink, &p->view, emit) + para;
    if (st->nopt) y += header(c, p, S_OPTIONS, y, w, emit);
    for (i = 0; i < st->nopt; i++) y += opt_row(c, p, i, y, w, emit);
    if (st->nsw) y += header(c, p, S_ARCHIVES, y, w, emit);
    for (i = 0; i < st->nsw; i++) y += sw_row(c, p, i, y, w, emit);
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
    tabs(c, body.x, body.y);
    panel(c, MODS_SCROLL_LIST, ui_rect(body.x, cy, t0, ch), list_rows, "scroll:list");
    panel(c, MODS_SCROLL_DETAIL, ui_rect(body.x + t0 + gap, cy, t1, ch), detail_rows, "scroll:detail");
    footer(c, body, cy + ch + gap);
}

/* ---- the entry points ---------------------------------------------------------------------------- */

/* MODS_MAXHOT, the bound: H is 720 units, so the pane's scroller is under 600 units tall once the
   window's frame, the tabs and the footer are off it, and one of its rows is at least layout.json's
   row.h + row.after, 17.5 today, which puts at most 35 rows on screen: 8 with a stepper and a reset, 3
   hits each, and 27 with a switch and a reset, 2 each, 78 in all. The list holds MODS_MAXROW rows of 2
   hits, 32; the tabs, the footer and the two scrollbars' tracks are 8. 118, under MODS_MAXHOT, so no
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

/* the fullest page there can be: every array full, every text at its array's length. `locked` greys
   every option and archive: most groups, fewest hits; else every one is changed: most hits. */
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
    s->nsw = MODS_MAXSW;
    s->update_all = 1;
    WORST(s->title, broken); WORST(s->meta, broken); WORST(s->summary, broken); WORST(s->note, broken); WORST(s->confirm, broken);
    for (i = 0; i < MODS_MAXROW; i++) {
        MODSROW* r = &s->row[i];
        WORST(r->name, broken); WORST(r->detail, broken); WORST(r->badge_text, broken);
        r->badge = (uint8_t)(1 + i % MODS_BADGE_CONFLICT);
        r->act = (uint8_t)(1 + i % MODS_ACT_BUSY);
        r->progress = 500;
    }
    for (i = 0; i < MODS_MAXOPT; i++) {
        MODSOPT* o = &s->opt[i];
        WORST(o->label, broken); WORST(o->value, broken); WORST(o->reason, broken);
        o->kind = MODS_OPT_STEPPER;
        o->changed = 1;
        o->locked = (uint8_t)locked;
    }
    for (i = 0; i < MODS_MAXSW; i++) {
        MODSSWITCH* w = &s->sw[i];
        WORST(w->name, broken);
        w->changed = 1;
        w->locked = (uint8_t)locked;
    }
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
    g->tab.line = g->foot.line = g->act.line = g->step.line = g->rst.line = g->line;
    for (i = 0; i < 4; i++) {
        g->wt[i] = ui_num(&x, "wt", NULL, i);
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
    g->marker = ui_num(&x, "marker", NULL, -1);
    g->check = ui_num(&x, "check", NULL, -1);
    g->radio = ui_num(&x, "radio", NULL, -1);
    g->act_w = ui_num(&x, "act", "w", -1);
    g->act.h = ui_num(&x, "act", "h", -1);
    g->act.pad = ui_num(&x, "act", "pad", -1);
    g->act.min = ui_num(&x, "act", "min", -1);
    g->step.h = g->rst.h = ui_num(&x, "small", "h", -1);
    g->step.pad = g->rst.pad = ui_num(&x, "small", "pad", -1);
    g->step.min = g->rst.min = ui_num(&x, "small", "min", -1);
    g->small_gap = ui_num(&x, "small", "gap", -1);
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
    l->g.step.style = l->role[R_STEP];
    l->g.rst.style = l->role[R_RESET];
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
