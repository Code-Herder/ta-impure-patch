/* tagpu_wikistate.c -- the in-game wiki's render-thread half. Contract: tagpu_wikistate.h. */
#include <windows.h>
#include <string.h>
#include "tagpu.h"
#include "tagpu_wikistate.h"
#include "tagpu_wikilay.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudart.h"
#include "tagpu_hudtext.h"
#include "tagpu_hud.h"
#include "tagpu_log.h"

static HUDOPS        s_ops;             /* the page's; 400 KB, never on a stack                  */
static WIKISTAGEOPS  s_stage;           /* the stage's; 0.9 MB                                   */
static WIKISTAGEVIEW s_view;            /* the record's stage, copied and bounded as data        */
static WIKIPAGE      s_page;
static WIKIFRAME     s_fr;
static int      s_have, s_haveStage;
static unsigned s_frame;                /* the render-thread frame the ops are for              */
static float    s_px;
static int      s_ox, s_oy;
static int      s_open, s_ptr;          /* the record's: the window open, a pointer in the picture */
static volatile LONG s_refused;         /* the layout refused: said once, drawn never; read by the
                                           game thread (tagpu_wikistate_refused)                */
static int      s_saidOver;             /* the page's ops overflowed: said once                 */
static int      s_drew;                 /* this frame's layer was drawn                         */
static TAGPU_CMD_WIKI s_map;
static int      s_mapCut;

/* ---- what the page and the stage need of the loaded art: set once, from the published art,
   which is immutable (tagpu_hudart.h) ---- */

static const WIKIART* s_art;
static WIKIFACES     s_faces;
static WIKISTAGEART  s_sart;
static int           s_toastFace;

static double stage_measure(void* ctx, double size, const char* s)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals(s_art->draw.lay);
    (void)ctx;
    return tagpu_hudtext_width(&s_art->draw.face[s_toastFace].info, (float)size, g->style[s_sart.style].spacing,
                               g->style[s_sart.style].upper, s, strlen(s));
}

static void stage_log(void* ctx, const char* line)
{
    (void)ctx;
    tagpu_logf("%s", line);
}

/* 0 when the art lacks what the stage draws with: the layout refuses the session then */
static int art_bind(const WIKIART* a)
{
    static const char* const CURSOR[3] = { "cursor_arrow", "cursor_move", "cursor_build" };
    const HUDGLOBALS* g = tagpu_hudlay_globals(a->draw.lay);
    int i;
    s_art = a;
    memset(&s_faces, 0, sizeof s_faces);
    for (i = 0; i < g->nstyle && i < 24; i++) {
        const HUDFACE* f = &a->draw.face[a->draw.style_face[i]];
        s_faces.face[i].info = &f->info;
        s_faces.face[i].ascent = f->ascent;
        s_faces.face[i].descent = f->descent;
    }
    memset(&s_sart, 0, sizeof s_sart);
    /* the stage's text is layout.json's `toast` style (stage.js FACE) */
    if ((s_sart.style = (int16_t)tagpu_hudlay_style_index(a->draw.lay, "toast")) < 0) return 0;
    s_toastFace = a->draw.style_face[s_sart.style];
    s_sart.measure = stage_measure;
    s_sart.log = stage_log;
    for (i = 0; i < 3; i++) {
        const int k = tagpu_hudlay_part_index(a->draw.lay, CURSOR[i]);
        const HUDPART* p = k >= 0 ? tagpu_hudlay_part(a->draw.lay, k) : NULL;
        s_sart.cursor[i] = (int16_t)k;
        if (p && p->scale > 0) { s_sart.cursor_w[i] = (float)p->pw / p->scale; s_sart.cursor_h[i] = (float)p->ph / p->scale; }
    }
    return 1;
}

/* THE RECORD'S STAGE IS DATA until bounded: every count held to its array, every string ended
   inside its own, so the build cannot read past the copy whatever the record held */
static void view_bound(WIKISTAGEVIEW* v)
{
    int i;
#define HOLD(n, max) do { if ((n) < 0) (n) = 0; if ((n) > (max)) (n) = (max); } while (0)
    HOLD(v->w, WIKISTAGE_MAP);
    HOLD(v->h, WIKISTAGE_MAP);
    HOLD(v->nvent, WIKISTAGE_VENTS);
    HOLD(v->nunit, WIKISTAGE_UNITS);
    HOLD(v->nsite, WIKISTAGE_SITES);
    HOLD(v->nqueued, WIKISTAGE_QUEUED);
    HOLD(v->nline, WIKISTAGE_LINES);
    HOLD(v->ntype, WIKI_MAX_DEFS);
#undef HOLD
    for (i = 0; i < v->nunit; i++) v->unit[i].label[WIKISTAGE_LABEL - 1] = 0;
    for (i = 0; i < WIKI_MAX_DEFS; i++) v->type[i][WIKISTAGE_LABEL - 1] = 0;
    v->caption[WIKISTAGE_CAPTION - 1] = 0;
}

/* the record into the layout's page state: the page's fields, the caption and the keys the demo's
   view holds */
static void page_of(WIKIPAGE* st, const TAGPU_PK_WIKI* w, const WIKISTAGEVIEW* v)
{
    int i;
    st->open = w->open;
    st->entry = w->entry;
    st->demo = w->demo;
    memcpy(st->fold, w->fold, sizeof st->fold < sizeof w->fold ? sizeof st->fold : sizeof w->fold);
    for (i = 0; i < WIKI_NSCROLL; i++) st->scroll[i] = w->scroll[i];
    st->playing = w->playing;
    st->done = w->done;
    st->speed = w->speed == 2 ? 2 : 1;
    st->try_it = w->try_it;
    st->mod = w->mod;
    memcpy(st->held, w->held, sizeof st->held);
    st->held[sizeof st->held - 1] = 0;
    st->caption[0] = 0;
    st->keys = 0;
    if (v) {
        if (v->has_caption) lstrcpynA(st->caption, v->caption, sizeof st->caption);
        for (i = 0; i < WIKI_NKEYS; i++)
            if (v->held[i]) st->keys |= 1u << i;
    }
}

void tagpu_wikistate_present(const TAGPU_FRAME* f)
{
    const struct TAGPU_PACKET* pk = f->packet;
    const TAGPU_PK_WIKI* w;
    const WIKISTAGEVIEW* sv;
    const WIKIART* art;
    const HUDSTATE* hs;
    char err[160], msg[sizeof w->msg];
    float u, hpx;
    int vw = f->vp_w, vh = f->vp_h, pw = 128, bh = 32, q8 = 256, hox, hoy;

    s_have = s_haveStage = 0;
    if (s_refused || !pk || !pk->in_game || !(w = tagpu_pk_wiki(pk))) return;
    /* THE ART LOADS ON ITS OWN THREAD (tagpu_hudart.c), started by whichever half asks first; the
       wiki draws from the first frame after it is published */
    if (!(art = tagpu_wikiart_get())) {
        tagpu_hudart_start();
        return;
    }
    if (art != s_art && !art_bind(art)) {
        InterlockedExchange(&s_refused, 1);
        tagpu_logf("wiki: the kit has no `toast` style for the stage - the wiki is not drawn this session");
        return;
    }
    if (vw > 0 && vh > 0) { s_ox = f->vp_x; s_oy = f->win_height - f->vp_y - vh; }
    else { vw = f->win_width; vh = f->win_height; s_ox = s_oy = 0; }
    if (f->game_width <= 0 || f->game_height <= 0 || vw <= 0 || vh <= 0 || s_oy < 0) return;

    /* THE WIKI UNIT is the screen's height / 720 device pixels, whatever the HUD scale (the
       mockup's --u); the button alone is placed in the top bar's own pixels, which the HUD scale
       sizes: the bar's height on screen (tagpu_hud_live, as tagpu_hudstate.c reads it) */
    u = (float)vh / 720.0f;
    tagpu_hud_live(&pw, &bh, &q8);
    sv = tagpu_pk_wiki_stage(pk);
    if (sv && w->has_stage) {
        memcpy(&s_view, sv, sizeof s_view);
        view_bound(&s_view);
        sv = &s_view;
    } else
        sv = NULL;
    memset(&s_page, 0, sizeof s_page);
    s_page.W = (float)vw;
    s_page.H = (float)vh;
    s_page.bar = (float)bh * (float)vh / (float)f->game_height / u;
    /* the rewind button's slot is taken when the same frame's live HUD draws its button */
    s_page.rewind = tagpu_hudstate_frame(f->frame_counter, &hs, &hpx, &hox, &hoy) && hs->live;
    page_of(&s_page, w, sv);
    if (!tagpu_wikilay_build(art->lay, art->data, &s_faces, &s_page, &s_ops, &s_fr, err, sizeof err)) {
        InterlockedExchange(&s_refused, 1);
        tagpu_logf("wiki: the page's layout refused (%s) - the wiki is not drawn this session", err);
        return;
    }
    if (s_ops.overflow && !s_saidOver) {
        s_saidOver = 1;
        tagpu_logf("wiki: the page's ops overflowed and some were dropped (said once)");
    }
    /* THE STAGE, inside the bezel the page placed, from the record's view or its message */
    if (w->open && s_fr.stage.w > 0 && s_fr.stage.h > 0) {
        memcpy(msg, w->msg, sizeof msg);
        msg[sizeof msg - 1] = 0;
        tagpu_wikistage_build(sv, msg[0] ? msg : NULL, s_fr.stage, u, &s_sart, &s_stage);
        s_haveStage = 1;
    }
    s_open = w->open;
    s_ptr = w->ptr_x >= 0 && w->ptr_y >= 0;
    s_px = u;
    s_frame = f->frame_counter;
    s_have = 1;
}

static HUDOPLIST stage_list(void)
{
    HUDOPLIST l;
    memset(&l, 0, sizeof l);
    l.op = s_stage.op;
    l.n = s_stage.n < WIKI_MAXOP ? s_stage.n : WIKI_MAXOP;
    l.run = s_stage.run;
    l.nrun = s_stage.nrun < WIKI_MAXRUN ? s_stage.nrun : WIKI_MAXRUN;
    l.text = s_stage.text;
    l.ntext = s_stage.ntext < WIKI_TEXTMAX ? s_stage.ntext : WIKI_TEXTMAX;
    return l;
}

int tagpu_wikistate_refused(void)
{
    return InterlockedCompareExchange(&s_refused, 0, 0) != 0;
}

int tagpu_wikistate_frame(unsigned frame, HUDOPLIST* page, HUDOPLIST* stage, float* px, int* ox, int* oy)
{
    if (!s_have || s_frame != frame) return 0;
    *page = tagpu_huddraw_list(&s_ops);
    if (s_haveStage) *stage = stage_list();
    else memset(stage, 0, sizeof *stage);
    *px = s_px;
    *ox = s_ox;
    *oy = s_oy;
    return 1;
}

/* a hit edge: the device pixel the draw puts the same edge on, from the screen's corner at `o` */
static int16_t dev(int o, double snapped)
{
    double r = o + snapped;
    return (int16_t)(r < -32768.0 ? -32768 : r > 32767.0 ? 32767 : r);
}

void tagpu_wikistate_drawn(int drew)
{
    int i, cut = 0;
    memset(&s_map, 0, offsetof(TAGPU_CMD_WIKI, hit));
    s_map.n = 0;
    s_drew = 0;
    if (!drew || !s_have) return;
    /* topmost first: the build paints in order, so the last control painted is on top. The
       stage holds no control */
    for (i = s_ops.n - 1; i >= 0; i--) {
        const HUDOP* p = &s_ops.op[i];
        TAGPU_CMD_WIKIHIT* t;
        if (p->kind != HOP_HOT) continue;
        if (s_map.n >= TAGPU_CMD_WIKIHITS) { cut++; continue; }
        t = &s_map.hit[s_map.n++];
        t->x0 = dev(s_ox, tagpu_hud_snap(p->r.x * s_px));
        t->y0 = dev(s_oy, tagpu_hud_snap(p->r.y * s_px));
        t->x1 = dev(s_ox, tagpu_hud_edge(p->r.x, p->r.w, s_px));
        t->y1 = dev(s_oy, tagpu_hud_edge(p->r.y, p->r.h, s_px));
        lstrcpynA(t->key, s_ops.text + p->run, sizeof t->key);
    }
    if (cut && !s_mapCut) {
        s_mapCut = 1;
        tagpu_logf("wiki: %d controls past the hit map's %u are not clickable (said once)", cut,
                   TAGPU_CMD_WIKIHITS);
    }
    /* the scrolls and the scrollbars as this frame drew them, which every scroll the game thread
       makes is held to */
    s_map.u = s_px;
    for (i = 0; i < WIKI_NSCROLL; i++) {
        const HUDRECT* r = &s_fr.track[i];
        TAGPU_CMD_WIKIBAR* b = &s_map.bar[i];
        s_map.scroll[i] = s_fr.scroll[i];
        s_map.scroll_max[i] = s_fr.scroll_max[i];
        if (r->h <= 0) continue;
        b->x0 = dev(s_ox, tagpu_hud_snap(r->x * s_px));
        b->y0 = dev(s_oy, tagpu_hud_snap(r->y * s_px));
        b->x1 = dev(s_ox, tagpu_hud_edge(r->x, r->w, s_px));
        b->y1 = dev(s_oy, tagpu_hud_edge(r->y, r->h, s_px));
        b->thumb_y = dev(s_oy, tagpu_hud_snap(s_fr.thumb_y[i] * s_px));
        b->thumb_h = dev(0, tagpu_hud_snap(s_fr.thumb_h[i] * s_px));
        /* the scroll a pixel of thumb travel is worth: the limit over the travel, Chrome's ratio */
        if ((r->h - s_fr.thumb_h[i]) * s_px > 0.5f)
            b->per_px = s_fr.scroll_max[i] / ((r->h - s_fr.thumb_h[i]) * s_px);
    }
    s_map.drawn = 1;
    s_map.open = (uint8_t)s_open;
    s_drew = 1;
}

int tagpu_wikistate_cursor(unsigned frame)
{
    return s_have && s_frame == frame && s_drew && (!s_open || s_ptr);
}

void tagpu_wikistate_cmd(TAGPU_CMD_WIKI* out)
{
    *out = s_map;
    memset(&s_map, 0, offsetof(TAGPU_CMD_WIKI, hit));
}
