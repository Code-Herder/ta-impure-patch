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
#include "tagpu_gaf.h"
#include "tagpu_gui.h"
#include "tagpu_pal.h"
#include "tagpu_wikiasset.h"
#include "tagpu_wikiside.h"

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
static int      s_tryPtr;               /* this frame is in Try it yourself with a view, and the
                                           record's pointer lies inside its stage               */
static int      s_curList, s_curAt;     /* the op that draws the stage's cursor: its list, 1 the
                                           stage's or 2 the ops over the image, 0 none; its index */
static int      s_stageCursor;          /* the HUD pass drew the stage's cursor this frame      */
static volatile LONG s_refused;         /* the layout refused: said once, drawn never; read by the
                                           game thread (tagpu_wikistate_refused)                */
static int      s_saidOver;             /* the page's ops overflowed: said once                 */
static int      s_drew;                 /* this frame's layer was drawn                         */
static TAGPU_CMD_WIKI s_map;
static int      s_mapCut;
/* the 3D stage: what tagpu_wikistate_scene answers, and the stage's ops split for its image */
static int      s_viewOk;               /* this frame's stage draws the record's view           */
static int      s_entry, s_demo;
static uint32_t s_shot;
static HUDRECT  s_stageRect;
static int      s_split;                /* tagpu_wikistate_image ran for s_frame                */
static HUDOP    s_over[WIKI_MAXOP];     /* the ops over the image, in their order; 0.8 MB       */
static int      s_nover;

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

/* ---- the game's cursors: the sprite sheet ------------------------------------------------------
   THE STAGE'S POINTER IS THE GAME'S OWN CURSOR, decoded here from the sequences the 3D asset load
   copied out of the player's cursors.GAF (tagpu_wikiasset.h, WIKI3D `cursor`), into the sheet the
   HUD pass draws as HUD_TEX_SPRITE (tagpu_vk_hud.c). Each frame through the PRESENTED palette, the
   one the sharp layer resolves the player's cursor through (tagpu_gui_surf.c sharp_cursor), its
   colour key transparent; premultiplied RGBA, one HUD_SPRITE_CELL cell a frame.

   SAFE BY CONSTRUCTION: the sequences lie in the asset arena, which is immutable once published
   and never freed, its every pointer written by the loader to a place inside it and its every
   count bounded against the file it came from; and each frame is bounded again here, its size by
   the cell and its count by the sheet, before a texel is written. The decode, the sheet and its
   generation are this thread's alone, as is the HUD pass that uploads it. A shape whose frames do
   not fit, or did not resolve, keeps n = 0 and draws its kit icon. */
static unsigned char  s_sprite[HUD_SPRITE_CELLS * HUD_SPRITE_CELL * HUD_SPRITE_CELL * 4];  /* 256 KB */
static unsigned char  s_dec[HUD_SPRITE_CELL * HUD_SPRITE_CELL];
static unsigned       s_spriteGen;      /* bumped at every rebuild; 0: no sheet                  */
static unsigned       s_spritePal;      /* the palette serial it was decoded through             */
static const WIKI3D*  s_spriteOf;       /* the load it was decoded from                          */
static WIKISTAGESEQ   s_seq[3];
static WIKISTAGESPR   s_spr[HUD_SPRITE_CELLS];
static int            s_saidUnfit;      /* a shape that does not fit was logged: said once, as each
                                           palette change decodes the same frames again          */

/* sequence `sq`'s frames into the sheet from cell `*cell`: 1 with `out` filled, 0 when one does not
   fit or does not decode (the cells it wrote are left to the next shape, which overwrites them) */
static int sprite_seq(const unsigned char* sq, const unsigned char* pal, int* cell, WIKISTAGESEQ* out)
{
    const int n = *(const uint16_t*)(sq + TAGPU_SQ_N);
    int k, x, y;
    if (n < 1 || n > HUD_SPRITE_CELLS - *cell) return 0;
    for (k = 0; k < n; k++) {
        const unsigned char* fr = *(const unsigned char* const*)(sq + TAGPU_SQ_TAB + 8 * k);
        const uint16_t dur = *(const uint16_t*)(sq + TAGPU_SQ_TAB + 8 * k + 4);
        TAGPU_GAFGEOM g;
        WIKISTAGESPR* s = &s_spr[*cell + k];
        if (!tagpu_gaf_frame_geom(fr, &g) || g.subn || g.w < 1 || g.h < 1 || g.w > HUD_SPRITE_CELL ||
            g.h > HUD_SPRITE_CELL || !tagpu_gaf_decode(fr, g.w, g.h, s_dec))
            return 0;
        for (y = 0; y < HUD_SPRITE_CELL; y++)
            for (x = 0; x < HUD_SPRITE_CELL; x++) {
                unsigned char* t = s_sprite + ((size_t)y * HUD_SPRITE_CELLS * HUD_SPRITE_CELL + (size_t)(*cell + k) * HUD_SPRITE_CELL + x) * 4;
                const int i = x < g.w && y < g.h ? s_dec[y * g.w + x] : g.ck;
                if (i == g.ck) { t[0] = t[1] = t[2] = t[3] = 0; continue; }
                t[0] = pal[i * 4];
                t[1] = pal[i * 4 + 1];
                t[2] = pal[i * 4 + 2];
                t[3] = 255;
            }
        s->w = (int16_t)g.w;
        s->h = (int16_t)g.h;
        s->hx = (int16_t)g.hotx;
        s->hy = (int16_t)g.hoty;
        s->dur = dur;
    }
    out->n = (uint8_t)n;
    out->first = (uint8_t)*cell;
    out->loop = sq[2];
    *cell += n;
    return 1;
}

/* the sheet for this frame: decoded once the load is published, again when the presented palette
   moves; the stage's art takes its table and the cursor's scale every frame */
static void sprites_bind(void)
{
    const WIKI3D* w3 = tagpu_wiki3d_get();
    const unsigned char* pal = tagpu_pal_live();
    const unsigned serial = tagpu_pal_serial();
    if (w3 && pal && (w3 != s_spriteOf || serial != s_spritePal)) {
        int k, j, cell = 0;
        memset(s_seq, 0, sizeof s_seq);
        memset(s_spr, 0, sizeof s_spr);
        for (k = 0; k < 3; k++) {
            for (j = 0; j < k && w3->cursor[j] != w3->cursor[k]; j++) {}
            if (j < k) s_seq[k] = s_seq[j];
            else if (w3->cursor[k] && !sprite_seq((const unsigned char*)w3->cursor[k], pal, &cell, &s_seq[k]) &&
                     !s_saidUnfit) {
                s_saidUnfit = 1;
                tagpu_logf("wiki: the game's cursor for shape %d does not fit the sprite sheet - its kit icon is "
                           "drawn (said once)", k);
            }
        }
        s_spriteOf = w3;
        s_spritePal = serial;
        if (++s_spriteGen == 0) s_spriteGen = 1;
    }
    memcpy(s_sart.seq, s_seq, sizeof s_sart.seq);
    memcpy(s_sart.spr, s_spr, sizeof s_sart.spr);
    s_sart.spr_scale = tagpu_gui_cursor_scale();
}

const unsigned char* tagpu_wikistate_sprites(unsigned* gen)
{
    *gen = s_spriteGen;
    return s_sprite;
}

/* ---- the sidebar ---------------------------------------------------------------------------------
   THE VIEW NAMES ITS LAYOUT BY A VALUE (WIKISTAGE_LAYOUT_*), resolved here to a layout this thread
   holds itself: the fixture of the wiki's data, published with the art this thread draws from, or
   the running mod's, read through tagpu_wikiside_get, whose interlocked read pairs with the
   loader's one publishing store, so the block is whole; both are immutable and never freed. The
   game thread names the mod's only after it read WIKISIDE_READY, which is final, so a READY is
   always here to resolve it; a record that says otherwise is resolved to no layout, and the stage
   draws the column and its minimap alone, as stage.js does with no layout. A source out of range
   draws no sidebar at all (view_bound). The mod's pictures are used only with the mod's layout. */
static const WIKILAYOUT* side_resolve(const WIKISTAGEVIEW* v, const WIKIART* art, const WIKISIDE** side)
{
    const WIKISIDE* s = NULL;
    *side = NULL;
    if (!v || !v->has_side) return NULL;
    if (v->layout_src == WIKISTAGE_LAYOUT_FIXTURE) return art->data ? art->data->fixture : NULL;
    if (v->layout_src != WIKISTAGE_LAYOUT_MOD || tagpu_wikiside_get(&s) != WIKISIDE_READY || !s) return NULL;
    *side = s;
    return &s->layout;
}

/* THE COMPOSED COLUMN (tagpu_wikiside.h), the texels of HUD_TEX_SIDE the stage's HOP_SIDE op draws:
   composed every frame from the frame's view, and its RGBA remade, with a new generation, only
   when the indices, the coverage, the rows or the art differ from the last frame's (FNV-1a 32 over
   them), which tagpu_vk_hud.c sends whole when the generation moves. This thread's alone. */
static WIKISIDECOL    s_col;                                      /* 256 KB               */
static unsigned char  s_colRgba[HUD_SIDE_W * HUD_SIDE_H * 4];     /* 512 KB               */
static uint32_t       s_colHash;
static const WIKISIDE* s_colOf;
static unsigned       s_colGen;         /* bumped at every new column; 0: none yet            */

typedef char side_texture_fits[HUD_SIDE_W == WIKI_SIDE_W && HUD_SIDE_H == WIKI_SIDE_ROWS ? 1 : -1];

static uint32_t fnv(uint32_t h, const unsigned char* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* this frame's column for view `v` on the mod's art, into s_sart: 1 when the stage draws it as
   its image, 0 for the whitebox column (no art, a view not on its layout, nothing composed) */
static int side_bind(const WIKISTAGEVIEW* v, const WIKIART* art, const WIKISIDE* side)
{
    const WIKI3D* w3 = tagpu_wiki3d_get();
    const WIKI3DSTAGE* st = NULL;
    uint32_t h;
    int i;
    s_sart.side = NULL;
    s_sart.side_minimap = 0;
    if (!side || !v) return 0;
    /* the stage's minimap picture: the asset arena's, by the entry's id, NULL until it is loaded */
    if (w3 && art->data && s_entry >= 0 && s_entry < art->data->nentry)
        for (i = 0; i < w3->nstage; i++)
            if (!strcmp(w3->stage[i].entry, art->data->entry[s_entry].id)) { st = &w3->stage[i]; break; }
    if (!tagpu_wikiside_compose(side, v, st ? st->minimap : NULL, st && st->minimap ? st->mm_w * st->mm_h : 0, &s_col))
        return 0;
    h = fnv(2166136261u, s_col.idx, (size_t)WIKI_SIDE_W * (size_t)s_col.h);
    h = fnv(h, s_col.a, (size_t)WIKI_SIDE_W * (size_t)s_col.h);
    h = fnv(h, (const unsigned char*)&s_col.h, sizeof s_col.h);
    if (h != s_colHash || side != s_colOf || !s_colGen) {
        memset(s_colRgba, 0, sizeof s_colRgba);
        tagpu_wikiside_rgba(side, &s_col, s_colRgba);
        s_colHash = h;
        s_colOf = side;
        if (++s_colGen == 0) s_colGen = 1;
    }
    s_sart.side = side;
    s_sart.side_minimap = s_col.minimap;
    return 1;
}

const unsigned char* tagpu_wikistate_side(unsigned* gen)
{
    *gen = s_colGen;
    return s_colRgba;
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
    /* the sidebar: a source no layout answers to draws none (side_resolve); its rows, states and
       the unit under the cursor held to their arrays, the locked type to the types */
    if (v->has_side != 1 || v->layout_src >= WIKISTAGE_LAYOUT_N) v->has_side = 0;
    HOLD(v->side.h, WIKI_SIDE_ROWS);
    HOLD(v->side.nstate, WIKI_SIDE_GADGETS);
    if (v->side.under < -1 || v->side.under >= v->nunit) v->side.under = -1;
    if (v->locked < -1 || v->locked >= v->ntype) v->locked = -1;
#undef HOLD
    for (i = 0; i < v->nunit; i++) v->unit[i].label[WIKISTAGE_LABEL - 1] = 0;
    for (i = 0; i < WIKI_MAX_DEFS; i++) v->type[i][WIKISTAGE_LABEL - 1] = 0;
    v->caption[WIKISTAGE_CAPTION - 1] = 0;
}

/* the record into the layout's page state: the page's fields, and the caption, the keys and the
   locked build the demo's view holds */
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
    st->sim_locked = v && v->locked >= 0;
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

    s_have = s_haveStage = s_viewOk = s_split = s_nover = s_curList = 0;
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
    s_entry = w->entry;
    s_demo = w->demo;
    s_shot = w->shot;
    s_open = w->open;
    s_ptr = w->ptr_x >= 0 && w->ptr_y >= 0;
    /* EXACTLY ONE CURSOR IN TRY IT YOURSELF (wiki.js draw): the stage draws the simulation's
       cursor only while the record's pointer lies inside the stage's canvas as this frame places
       it, the client pixels of the stage's hit rect (tagpu_wikistate_drawn), and the player's is
       hidden only in a frame whose HUD pass drew the stage's (tagpu_wikistate_cursor). Every fact
       is of this one frame, so none shows both cursors or neither. A demo draws its scripted
       cursor wherever the pointer is. */
    s_tryPtr = 0;
    if (w->open && s_fr.stage.w > 0 && s_fr.stage.h > 0) {
        double cx, cy, cw, ch;
        tagpu_wikistage_canvas(s_fr.stage, u, &cx, &cy, &cw, &ch);
        cx += s_ox;
        cy += s_oy;
        s_viewOk = sv && sv->w > 0 && sv->h > 0;
        s_tryPtr = w->try_it && s_viewOk && s_ptr && w->ptr_x >= cx && w->ptr_x < cx + cw &&
                   w->ptr_y >= cy && w->ptr_y < cy + ch;
        /* THE STAGE, inside the bezel the page placed, from the record's view or its message */
        memcpy(msg, w->msg, sizeof msg);
        msg[sizeof msg - 1] = 0;
        sprites_bind();
        {
            const WIKISIDE* side;
            s_sart.side_layout = side_resolve(sv, art, &side);
            side_bind(sv, art, side);
        }
        tagpu_wikistage_build(sv, msg[0] ? msg : NULL, s_fr.stage, u, !w->try_it || s_tryPtr, &s_sart, &s_stage);
        s_haveStage = 1;
        s_stageRect = s_fr.stage;
        if (s_stage.cursor >= 0 && s_stage.cursor < s_stage.n && s_stage.cursor < WIKI_MAXOP) {
            s_curList = 1;
            s_curAt = s_stage.cursor;
        }
    }
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
    /* the whitebox sidebar's greyed buttons, one group each: without them the draw skips their ops */
    l.grp = s_stage.grp;
    l.ngrp = s_stage.ngrp < WIKI_MAXGRP ? s_stage.ngrp : WIKI_MAXGRP;
    return l;
}

int tagpu_wikistate_scene(unsigned frame, const WIKISTAGEVIEW** v, int* entry, int* demo, uint32_t* shot,
                          HUDRECT* stage, float* px, int* ox, int* oy)
{
    if (!s_have || s_frame != frame || !s_viewOk) return 0;
    *v = &s_view;
    *entry = s_entry;
    *demo = s_demo;
    *shot = s_shot;
    *stage = s_stageRect;
    *px = s_px;
    *ox = s_ox;
    *oy = s_oy;
    return 1;
}

/* THE SPLIT IS IN PLACE, ONCE: the ops kept under the image close up at the front of the stage's
   own array, in their order (an op only ever moves to an index at or before its own, so each is
   read before anything is written over it), and those over it are copied out in theirs. The
   runs and the text stay where they are, and every op still names its own. */
void tagpu_wikistate_image(unsigned frame, int image, int ground, const uint32_t* resolved)
{
    int i, n, under = 0;
    if (!s_have || s_frame != frame || !s_haveStage || s_split) return;
    s_split = 1;
    if (!image) return;
    n = s_stage.n < WIKI_MAXOP ? s_stage.n : WIKI_MAXOP;
    for (i = 0; i < n; i++) {
        const int k = tagpu_wikistage_keep(&s_stage, i, 1, ground, resolved);
        if (i == s_curAt && s_curList) {
            s_curList = k;
            s_curAt = k == 1 ? under : s_nover;
        }
        if (k == 1) s_stage.op[under++] = s_stage.op[i];
        else if (k == 2) s_over[s_nover++] = s_stage.op[i];
    }
    s_stage.n = under;
}

int tagpu_wikistate_refused(void)
{
    return InterlockedCompareExchange(&s_refused, 0, 0) != 0;
}

int tagpu_wikistate_frame(unsigned frame, HUDOPLIST* page, HUDOPLIST* stage, HUDOPLIST* over, float* px,
                          int* ox, int* oy)
{
    if (!s_have || s_frame != frame) return 0;
    *page = tagpu_huddraw_list(&s_ops);
    memset(stage, 0, sizeof *stage);
    memset(over, 0, sizeof *over);
    stage->watch = over->watch = -1;
    if (s_haveStage) {
        *stage = stage_list();
        stage->watch = s_curList == 1 ? s_curAt : -1;
        if (s_nover) {
            *over = *stage;
            over->op = s_over;
            over->n = s_nover;
            over->watch = s_curList == 2 ? s_curAt : -1;
        }
    }
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

void tagpu_wikistate_drawn(int drew, int cursor)
{
    int i, cut = 0;
    memset(&s_map, 0, offsetof(TAGPU_CMD_WIKI, hit));
    s_map.n = 0;
    s_drew = s_stageCursor = 0;
    if (!drew || !s_have) return;
    s_stageCursor = cursor;
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
    s_map.entry = (int16_t)s_page.entry;
    s_drew = 1;
}

int tagpu_wikistate_cursor(unsigned frame)
{
    return s_have && s_frame == frame && s_drew && (!s_open || (s_ptr && !(s_tryPtr && s_stageCursor)));
}

void tagpu_wikistate_cmd(TAGPU_CMD_WIKI* out)
{
    *out = s_map;
    memset(&s_map, 0, offsetof(TAGPU_CMD_WIKI, hit));
}
