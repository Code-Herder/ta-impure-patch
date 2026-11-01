/* tagpu_wikistate.c -- the in-game wiki's render-thread half. Contract: tagpu_wikistate.h. */
#include <windows.h>
#include <math.h>
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
#include "tagpu_wikibar.h"
#include "tagpu_wikiframe.h"

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
static int      s_viewSide;             /* and that view's sidebar rows, 0 without one          */
static int      s_entry, s_demo;
static uint32_t s_shot;
static HUDRECT  s_stageRect;
static int      s_split;                /* tagpu_wikistate_image ran for s_frame                */
static HUDOP    s_over[WIKI_MAXOP];     /* the ops over the image, in their order; 0.8 MB       */
static int      s_nover;

/* ---- a frame:game stage: its record and its HUD --------------------------------------------------
   THE STAGE HUD IS THIS THREAD'S OWN: its state and its ops are these statics, never the replay
   HUD's (tagpu_hudstate.c's state, tagpu_vk_hud.c's ops), which are in use when the wiki is open
   over a replay; the game thread never reads them, and their hits enter the wiki's map only, never
   tagpu_hudstate_drawn's, so the stage can never drive the real replay. */
#define WIKISTATE_TOPMAX 64             /* the frame's TOP range: Leave's question and the viewer's
                                           cursor, about a dozen ops (tagpu_wikistage.c frame_top) */
static WIKIFRAMEREC s_rec;              /* the record's frame, copied and bounded as data        */
static uint32_t     s_repSerial;        /* the packet's rep_serial, of the frame s_rec is from   */
static int          s_frameOn;          /* this frame's stage is a game frame with its record     */
static HUDSTATE     s_hst;              /* 35 KB                                                  */
static HUDSTATE     s_pst;              /* a return glide's pressed HUD (tagpu_wikiframe_pressed) */
static HUDOPS       s_hops;             /* 0.5 MB                                                 */
static int          s_hudOn;            /* s_hops holds this frame's stage HUD                    */
static float        s_hpx;              /* its unit: the frame's height / 720 device pixels       */
static int          s_hox, s_hoy;       /* the frame's corner on the attachment                   */
static int          s_cv[4];            /* the stage's canvas on the attachment: x0 y0 x1 y1      */
static WIKIFRAMEDLG s_dlg;              /* Leave's question, canvas device pixels                 */
static int          s_dlgX, s_dlgY;     /* the canvas's corner on the attachment                  */
static HUDOP        s_top[WIKISTATE_TOPMAX];
static int          s_ntop, s_saidTop;
/* THE FRAME'S BARS (tagpu_wikibar.h): composed when what they are composed of changes -- the
   published sidebar art, which is immutable, and the side, the colour and the frame's size -- and
   only then, which is exact, since the composer is pure and its numbers are fixed: the viewed
   record is the viewer's, whose every number is 0 (presentation spec 3.5). A new composition moves
   the generation, which tagpu_vk_hud.c sends whole. This thread's alone. */
static uint8_t          s_barIdx[WIKIBAR_W * WIKIBAR_H];          /* 128 KB               */
static unsigned char    s_barRgba[WIKIBAR_W * WIKIBAR_H * 4];     /* 512 KB               */
static unsigned         s_barGen;       /* bumped at every new composition; 0: none yet       */
static int              s_barW;         /* its width, -1 when the key composes nothing        */
static const WIKISIDE*  s_barOf;
static int              s_barSide = -2, s_barColour = -1, s_barWg, s_barHg;
static WIKIFRAMEBAR     s_barNum;       /* the numbers composed                               */
/* what the stage HUD costs this thread, told when the window closes */
static LARGE_INTEGER    s_qpf;
static double           s_costHud, s_costBar;
static unsigned         s_costFrames;

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
    /* the kit's icons by WIKI_CURSOR_* (stage.js CURSOR): reclaim draws the arrow's */
    static const char* const CURSOR[WIKI_CURSOR_N] = { "cursor_arrow", "cursor_move", "cursor_build", "cursor_arrow" };
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
    for (i = 0; i < WIKI_CURSOR_N; i++) {
        const int k = tagpu_hudlay_part_index(a->draw.lay, CURSOR[i]);
        const HUDPART* p = k >= 0 ? tagpu_hudlay_part(a->draw.lay, k) : NULL;
        s_sart.cursor[i] = (int16_t)k;
        if (p && p->scale > 0) { s_sart.cursor_w[i] = (float)p->pw / p->scale; s_sart.cursor_h[i] = (float)p->ph / p->scale; }
    }
    /* the game frame's emblems in its top bar (stage.js EMBLEM) */
    s_sart.emblem[0] = (int16_t)tagpu_hudlay_part_index(a->draw.lay, "wiki_emblem");
    s_sart.emblem[1] = (int16_t)tagpu_hudlay_part_index(a->draw.lay, "config_emblem");
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
static unsigned char  s_sprite[HUD_SPRITE_CELLS * HUD_SPRITE_CELL * HUD_SPRITE_CELL * 4];  /* 512 KB */
static unsigned char  s_dec[HUD_SPRITE_CELL * HUD_SPRITE_CELL];
static unsigned       s_spriteGen;      /* bumped at every rebuild; 0: no sheet                  */
static unsigned       s_spritePal;      /* the palette serial it was decoded through             */
static const WIKI3D*  s_spriteOf;       /* the load it was decoded from                          */
static WIKISTAGESEQ   s_seq[WIKI_CURSOR_N];
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
        for (k = 0; k < WIKI_CURSOR_N; k++) {
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
   composed every frame from the frame's view into the one of two buffers that does not hold the
   column drawn last, and compared with that one byte for byte -- its rows, its indices and its
   coverage -- so the RGBA is remade, with a new generation that tagpu_vk_hud.c sends whole, exactly
   when a texel can differ, never on a hash's say-so: a frame never shows another frame's column as
   its own. The comparison is two memcmp over at most 2 x 128 x WIKI_SIDE_ROWS bytes on columns
   that agree: MEASURED 2026-10-27 at 5.9 us for a 480-row column and 12.9 us for 1024 rows, i686
   -O2 -msse2 under Wine on the reference setup (1.5 and 3.2 us native x86-64). This thread's alone. */
static WIKISIDECOL    s_col[2];                                   /* 192 KB each          */
static int            s_colCur;         /* s_col[s_colCur] is the column s_colRgba holds        */
static unsigned char  s_colRgba[HUD_SIDE_W * HUD_SIDE_H * 4];     /* 512 KB               */
static const WIKISIDE* s_colOf;
static unsigned       s_colGen;         /* bumped at every new column; 0: none yet            */

typedef char side_texture_fits[HUD_SIDE_W == WIKI_SIDE_W && HUD_SIDE_H == WIKI_SIDE_ROWS ? 1 : -1];

/* 1 when columns `a` and `b` give the same texels: the same rows, and the same indices and
   coverage in them (the texels below the rows are transparent in both) */
static int col_same(const WIKISIDECOL* a, const WIKISIDECOL* b)
{
    const size_t n = (size_t)WIKI_SIDE_W * (size_t)a->h;
    return a->h == b->h && !memcmp(a->idx, b->idx, n) && !memcmp(a->a, b->a, n);
}

/* this frame's column for view `v` on the mod's art, into s_sart: 1 when the stage draws it as
   its image, 0 for the whitebox column (no art, a view not on its layout, nothing composed) */
static int side_bind(const WIKISTAGEVIEW* v, const WIKIART* art, const WIKISIDE* side)
{
    const WIKI3D* w3 = tagpu_wiki3d_get();
    const WIKI3DSTAGE* st = NULL;
    WIKISIDECOL* c = &s_col[s_colCur ^ 1];
    s_sart.side = NULL;
    s_sart.side_minimap = 0;
    if (!side || !v) return 0;
    /* the stage's minimap picture: the asset arena's, the demo's scene's, NULL until it is loaded */
    if (w3 && art->data && s_entry >= 0 && s_entry < art->data->nentry)
        st = tagpu_wiki3d_stage(w3, &art->data->entry[s_entry], s_demo);
    /* a stage's minimap picture is of one map's rows: the view's map takes it only when they are its */
    if (st && (!st->minimap || st->mm_rows != v->h)) st = NULL;
    /* the side the view shows, whose PANELSIDE is drawn: data, bounded by the composer against the
       layout's sides */
    if (!tagpu_wikiside_compose(side, v, v->side.index, st ? st->minimap : NULL, st ? st->mm_w * st->mm_h : 0, c))
        return 0;
    if (!s_colGen || side != s_colOf || !col_same(c, &s_col[s_colCur])) {
        memset(s_colRgba, 0, sizeof s_colRgba);
        tagpu_wikiside_rgba(side, c, s_colRgba);
        s_colCur ^= 1;
        s_colOf = side;
        if (++s_colGen == 0) s_colGen = 1;
    }
    s_sart.side = side;
    s_sart.side_minimap = c->minimap;
    return 1;
}

const unsigned char* tagpu_wikistate_side(unsigned* gen)
{
    *gen = s_colGen;
    return s_colRgba;
}

const unsigned char* tagpu_wikistate_bar(unsigned* gen)
{
    *gen = s_barGen;
    return s_barRgba;
}

/* the bars for the bounded record on `side`'s art in a frame Wg x Hg, showing its numbers (a live
   frame's memo, Watching's zeros): their width, 0 for the whitebox strips. Composed again only when
   the art, the side, the badge, the size or a number changes. A live frame's numbers include seat
   0's stored values, which move whenever the economy flows, so its bars are composed again about
   once a game tick; Watching's are zeros and compose once. */
static int bar_bind(const WIKISIDE* side, int Wg, int Hg)
{
    if (!side) return 0;
    if (side != s_barOf || s_rec.side != s_barSide || s_rec.colour != s_barColour || Wg != s_barWg || Hg != s_barHg ||
        memcmp(&s_rec.bar, &s_barNum, sizeof s_barNum)) {
        const WIKIFRAMEBAR* b = &s_rec.bar;
        WIKIBARNUM n;
        memset(&n, 0, sizeof n);
        n.e = b->e; n.m = b->m; n.emax = b->emax; n.mmax = b->mmax;
        n.ei = b->ei; n.eo = b->eo; n.mi = b->mi; n.mo = b->mo;
        n.raw_e = b->raw_e; n.raw_m = b->raw_m;
        s_barOf = side;
        s_barSide = s_rec.side;
        s_barColour = s_rec.colour;
        s_barWg = Wg;
        s_barHg = Hg;
        s_barNum = s_rec.bar;
        s_barW = s_rec.side < 0 ? -1 : tagpu_wikibar_compose(side, s_rec.side, s_rec.colour, Wg, Hg, &n, s_barIdx);
        if (s_barW > 0) {
            tagpu_wikibar_rgba(side, s_barIdx, s_barW, s_barRgba);
            if (++s_barGen == 0) s_barGen = 1;
        }
    }
    return s_barW > 0 && s_barGen ? s_barW : 0;
}

static double elapsed_ms(const LARGE_INTEGER* t0)
{
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return s_qpf.QuadPart > 0 ? (double)(t1.QuadPart - t0->QuadPart) * 1000.0 / (double)s_qpf.QuadPart : 0;
}

/* THE STAGE HUD of a frame view `v` on the canvas of this frame's stage at `u`, from the bounded
   record (presentation spec 5.4): the fit, the frame's size and its HUD unit, the HUD's corner --
   the screen's corner, the canvas's and the frame's snapped one, all whole device pixels, so the
   HUD's own snap rules give the pixels the replay HUD gives on a screen of the frame's size --
   build 1 for the boxes, the viewer's point on them, build 2 with the point (a demo) or the
   player's pointer (Try it) for the hint, which is drawn last and moves no box
   (tagpu_hudlay.c hint), and the bars. Fills `sf` for the stage's build. With no HUD art yet the
   stage draws no HUD and takes no hit on it this frame. */
static void frame_hud(const WIKISTAGEVIEW* v, const WIKISIDE* side, float u, const TAGPU_PK_WIKI* w, WIKISTAGEFRAME* sf)
{
    const HUDART* hart = tagpu_hudart_get();
    const WIKIART* art = s_art;
    WIKISTAGEFIT fit;
    LARGE_INTEGER t0;
    char err[160];
    double X0, Y0, cw, ch, fx, fy, x, y;
    int Wg, Hg, simDrawn = 0;
    QueryPerformanceCounter(&t0);
    tagpu_wikistage_canvas(s_fr.stage, u, &X0, &Y0, &cw, &ch);
    tagpu_wikistage_fit(v->w, v->h, v->has_side ? v->side.h : 0, 1, cw, ch, u, &fit);
    tagpu_wikiframe_size(v->w, v->h, v->has_side ? v->side.h : 0, &Wg, &Hg);
    tagpu_wikiframe_dialog(&fit, Wg, Hg, &s_dlg);
    fx = tagpu_hud_snap((float)(fit.ox + fit.x0 * fit.s));
    fy = tagpu_hud_snap((float)(fit.oz + fit.z0 * fit.s));
    s_hpx = (float)(Hg * fit.s / WIKIFRAME_HUD_H);
    s_dlgX = s_ox + (int)floor(X0 + 0.5);
    s_dlgY = s_oy + (int)floor(Y0 + 0.5);
    s_hox = s_dlgX + (int)fx;
    s_hoy = s_dlgY + (int)fy;
    s_cv[0] = s_dlgX;
    s_cv[1] = s_dlgY;
    s_cv[2] = s_dlgX + (int)floor(cw + 0.5);
    s_cv[3] = s_dlgY + (int)floor(ch + 0.5);
    memset(sf, 0, sizeof *sf);
    sf->cursor_x = sf->cursor_y = NAN;
    sf->question = s_rec.question;
    sf->held = (uint8_t)(w->leave_held <= 2 ? w->leave_held : 0);
    if (art && art->lay) {
        double side_, top, right, config;
        tagpu_wikilay_button(art->lay, s_rec.live, &side_, &top, &right, &config);
        sf->btn_side = (int16_t)side_;
        sf->btn_top = (int16_t)top;
        sf->btn_right = (int16_t)right;
        sf->btn_config = (int16_t)config;
    }
    if (hart && s_hpx > 0) {
        tagpu_wikiframe_hudstate(&s_rec, Wg, Hg, s_hpx, side, &s_hst);
        s_hst.mx = s_hst.my = -1;
        if (tagpu_hudlay_build(hart->lay, &s_hst, &s_hops, err, sizeof err)) {
            WIKIFRAMEREC pr;
            double gx, gy;
            int rebuild = 0;
            s_hudOn = 1;
            tagpu_wikiframe_game_point(&s_rec, &fit, fx, fy, s_hpx, &gx, &gy);
            /* a return glide leaves a control its press closed: it is resolved among the HUD's
               controls as drawn at that press, and the HUD is laid out again as drawn now */
            if (s_rec.cur.on && tagpu_wikiframe_pressed(&s_rec, &pr)) {
                tagpu_wikiframe_hudstate(&pr, Wg, Hg, s_hpx, side, &s_pst);
                s_pst.mx = s_pst.my = -1;
                if (!tagpu_hudlay_build(hart->lay, &s_pst, &s_hops, err, sizeof err)) s_hudOn = 0;
                rebuild = 1;
            }
            if (s_rec.cur.on && s_hudOn) {
                /* the rest point is the map's centre; No is the question's, NaN while it is not drawn */
                const double rx = (fit.ox + v->w * WIKI_CELL / 2.0 * fit.s - fx) / s_hpx;
                const double ry = (fit.oz + v->h * WIKI_CELL / 2.0 * fit.s - fy) / s_hpx;
                const double nx = s_rec.question ? (s_dlg.no.x + s_dlg.no.w / 2.0 - fx) / s_hpx : NAN;
                const double ny = s_rec.question ? (s_dlg.no.y + s_dlg.no.h / 2.0 - fy) / s_hpx : NAN;
                tagpu_wikiframe_point(&s_rec, &s_hops, s_hpx, tagpu_hudlay_globals(hart->lay)->track_ends, rx, ry, nx, ny,
                                      gx, gy, &x, &y);
                s_hst.mx = (float)x;
                s_hst.my = (float)y;
                sf->cursor_x = fx + x * s_hpx;
                sf->cursor_y = fy + y * s_hpx;
            } else if (s_rec.simcur && !w->try_it && gx == gx && gy == gy) {
                /* THE ONE CURSOR: a demo's simulation's own, its shape and animation at its world
                   point; Try it's follows the player's pointer (below) */
                s_hst.mx = (float)gx;
                s_hst.my = (float)gy;
                sf->cursor_x = fx + gx * s_hpx;
                sf->cursor_y = fy + gy * s_hpx;
                sf->cursor_shape = s_rec.sim.shape;
                sf->cursor_age = s_rec.sim.age;
            } else if (w->try_it && w->ptr_x >= s_cv[0] && w->ptr_x < s_cv[2] && w->ptr_y >= s_cv[1] && w->ptr_y < s_cv[3]) {
                s_hst.mx = (float)((w->ptr_x - s_hox) / (double)s_hpx);
                s_hst.my = (float)((w->ptr_y - s_hoy) / (double)s_hpx);
                /* THE ONE CURSOR in Try it: after a fork the simulation's follows the player's pointer
                   over the world and is the one drawn there, its shape and animation at the pointer
                   (wiki.js drawReplay). Whether the pointer is on the world is the game thread's one
                   answer (TAGPU_PK_WIKI.try_world), which also kept the view's ghost, so the two are
                   never decided apart; the player's own is hidden only in a frame this draws
                   (tagpu_wikistate_cursor) */
                if (s_rec.simcur && w->try_world == 1) {
                    sf->cursor_x = w->ptr_x - s_dlgX;
                    sf->cursor_y = w->ptr_y - s_dlgY;
                    sf->cursor_shape = s_rec.sim.shape;
                    sf->cursor_age = s_rec.sim.age;
                    simDrawn = 1;
                }
            }
            if (s_hudOn && (rebuild || s_hst.mx >= 0) && !tagpu_hudlay_build(hart->lay, &s_hst, &s_hops, err, sizeof err))
                s_hudOn = 0;
        }
        if (!s_hudOn) {
            InterlockedExchange(&s_refused, 1);
            tagpu_logf("wiki: the stage HUD's layout refused (%s) - the wiki is not drawn this session", err);
        }
    }
    /* THE GHOST GOES WITH THE CURSOR: try_world, which kept the view's ghost, is the game
       thread's answer through an earlier frame's map, while the cursor above is placed on this
       frame's canvas, so after a resize the two can differ for a frame. The ghost is drawn only
       in a frame that draws the simulation's cursor, so no frame shows it beside the player's
       arrow (s_view is this thread's copy; the stage and the 3D scene both read it after this). */
    if (w->try_it && w->try_world == 1 && !simDrawn) s_view.has_ghost = 0;
    {
        const double hud = elapsed_ms(&t0);
        QueryPerformanceCounter(&t0);
        sf->bar_w = (int16_t)bar_bind(side, Wg, Hg);
        s_costBar = fmax(s_costBar, elapsed_ms(&t0));
        s_costHud = fmax(s_costHud, hud);
        s_costFrames++;
    }
}

/* THE TOP RANGE OUT OF THE STAGE'S OPS, in place and in order (as tagpu_wikistate_image splits):
   Leave's question and the viewer's cursor, drawn after the stage HUD whatever the image. An op
   past WIKISTATE_TOPMAX is dropped, said once. */
static void top_split(void)
{
    int i, n = s_stage.n < WIKI_MAXOP ? s_stage.n : WIKI_MAXOP, keep = 0, cut = 0;
    s_ntop = 0;
    for (i = 0; i < n; i++) {
        if (s_stage.range[i] != WIKISTAGE_IN_TOP) {
            if (i == s_curAt && s_curList == 1) s_curAt = keep;
            s_stage.range[keep] = s_stage.range[i];
            s_stage.rtype[keep] = s_stage.rtype[i];
            s_stage.op[keep++] = s_stage.op[i];
        } else if (s_ntop < WIKISTATE_TOPMAX) {
            if (i == s_curAt && s_curList == 1) {
                s_curList = 3;
                s_curAt = s_ntop;
            }
            s_top[s_ntop++] = s_stage.op[i];
        } else {
            if (i == s_curAt && s_curList == 1) s_curList = 0;
            cut++;
        }
    }
    s_stage.n = keep;
    if (cut && !s_saidTop) {
        s_saidTop = 1;
        tagpu_logf("wiki: %d ops of the frame's question and cursor past %d were dropped (said once)", cut,
                   WIKISTATE_TOPMAX);
    }
}

/* THE RECORD'S STAGE IS DATA until bounded: every count held to its array, every string ended
   inside its own, so the build cannot read past the copy whatever the record held */
static void view_bound(WIKISTAGEVIEW* v)
{
    int i;
#define HOLD(n, max) do { if ((n) < 0) (n) = 0; if ((n) > (max)) (n) = (max); } while (0)
    HOLD(v->w, WIKISTAGE_MAP);
    HOLD(v->h, WIKISTAGE_MAP);
    v->frame = (uint8_t)(v->frame != 0);
    HOLD(v->nvent, WIKISTAGE_VENTS);
    HOLD(v->nunit, WIKISTAGE_UNITS);
    HOLD(v->nsite, WIKISTAGE_SITES);
    HOLD(v->nqueued, WIKISTAGE_QUEUED);
    HOLD(v->nline, WIKISTAGE_LINES);
    HOLD(v->ntype, WIKI_MAX_DEFS);
    if (v->npart > WIKISTAGE_PARTS) v->npart = WIKISTAGE_PARTS;
    /* the features and the sweep: the counts to their arrays; the build bounds each target, the
       hover and the kind it reads */
    HOLD(v->nfeat, WIKISTAGE_FEATS);
    HOLD(v->ngone, WIKISTAGE_FEATS);
    HOLD(v->sweep.ntarget, WIKISTAGE_FEATS);
    /* the sidebar: a source no layout answers to draws none (side_resolve); its rows, states and
       the unit under the cursor held to their arrays, the locked type to the types */
    if (v->has_side != 1 || v->layout_src >= WIKISTAGE_LAYOUT_N) v->has_side = 0;
    HOLD(v->side.h, WIKI_SIDE_ROWS);
    HOLD(v->side.nstate, WIKI_SIDE_GADGETS);
    if (v->side.index < -1 || v->side.index >= WIKI_SIDE_SIDES) v->side.index = -1;
    if (v->side.under < -1 || v->side.under >= v->nunit) v->side.under = -1;
    if (v->locked != WIKI_LOCK_RECLAIM && (v->locked < -1 || v->locked >= v->ntype)) v->locked = -1;
#undef HOLD
    for (i = 0; i < v->nunit; i++) v->unit[i].label[WIKISTAGE_LABEL - 1] = 0;
    for (i = 0; i < WIKI_MAX_DEFS; i++) v->type[i][WIKISTAGE_LABEL - 1] = 0;
    v->caption[WIKISTAGE_CAPTION - 1] = 0;
}

/* THE CASTING THE TEXT IS FILLED WITH, from the record's layout side: the published WIKISIDE's,
   read through tagpu_wikiside_get, whose interlocked read pairs with the loader's one publishing
   store, so the block is whole; it is immutable and never freed, so the pointer holds for the
   process. The game thread names a side only after it read WIKISIDE_READY, which is final; the
   side is data all the same, bounded against WIKI_SIDE_SIDES and the layout's sides, and any
   other value, or no published block, fills with each entry's own names. */
static const WIKICASTING* casting_of(int side)
{
    const WIKISIDE* s = NULL;
    if (side < 0 || side >= WIKI_SIDE_SIDES || tagpu_wikiside_get(&s) != WIKISIDE_READY || !s || side >= s->layout.nside)
        return NULL;
    return &s->casting[side];
}

/* the record into the layout's page state: the page's fields, the context its text is filled
   with, and the caption, the keys and the locked build the demo's view holds */
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
    st->config = w->config <= 2 ? w->config : 0;
    st->about = w->about != 0;
    st->drawer = w->drawer != 0;
    st->iface = w->iface == 1;
    st->casting = casting_of(w->cast_side);
    memcpy(st->held, w->held, sizeof st->held);
    st->held[sizeof st->held - 1] = 0;
    st->caption[0] = 0;
    st->keys = 0;
    st->sim_locked = v && (v->locked >= 0 || v->locked == WIKI_LOCK_RECLAIM);
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
    const WIKIFRAMEREC* rec;
    const WIKISIDE* side = NULL;
    WIKISTAGEFRAME sf;
    const WIKIART* art;
    const HUDSTATE* hs;
    char err[160], msg[sizeof w->msg];
    float u, hpx;
    int vw = f->vp_w, vh = f->vp_h, pw = 128, bh = 32, q8 = 256, hox, hoy;

    s_have = s_haveStage = s_viewOk = s_viewSide = s_split = s_nover = s_curList = 0;
    s_frameOn = s_hudOn = s_ntop = 0;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
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
    s_repSerial = w->rep_serial;
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
    /* the stage HUD's cost, told when the window closes (lane watch reads it) */
    if (!w->open && s_costFrames) {
        tagpu_logf("wiki: the stage HUD cost the render thread at most %.3f ms in one frame (its two builds), "
                   "the bars at most %.3f ms, over %u frames", s_costHud, s_costBar, s_costFrames);
        s_costHud = s_costBar = 0;
        s_costFrames = 0;
    }
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
        s_viewSide = s_viewOk && sv->has_side ? sv->side.h : 0;
        s_tryPtr = w->try_it && s_viewOk && s_ptr && w->ptr_x >= cx && w->ptr_x < cx + cw &&
                   w->ptr_y >= cy && w->ptr_y < cy + ch;
        /* THE STAGE, inside the bezel the page placed, from the record's view or its message */
        memcpy(msg, w->msg, sizeof msg);
        msg[sizeof msg - 1] = 0;
        sprites_bind();
        s_sart.side_layout = side_resolve(sv, art, &side);
        side_bind(sv, art, side);
        /* A GAME FRAME'S RECORD, carried exactly with the stage: copied, then bounded before any
           read, against the published side's layout and badge frames, or the whitebox's */
        if (sv && sv->frame && (rec = tagpu_pk_wiki_frame(pk)) != NULL && rec->on) {
            memcpy(&s_rec, rec, sizeof s_rec);
            tagpu_wikiframe_bound(&s_rec, side ? side->layout.nside : WIKI_SIDE_SIDES, side ? side->nlogo : WIKI_LOGOS);
            s_frameOn = 1;
            frame_hud(sv, side, u, w, &sf);
        }
        tagpu_wikistage_build(sv, msg[0] ? msg : NULL, s_fr.stage, u, !w->try_it || s_tryPtr, s_frameOn ? &sf : NULL,
                              &s_sart, &s_stage);
        s_haveStage = 1;
        s_stageRect = s_fr.stage;
        if (s_stage.cursor >= 0 && s_stage.cursor < s_stage.n && s_stage.cursor < WIKI_MAXOP) {
            s_curList = 1;
            s_curAt = s_stage.cursor;
        }
        top_split();
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

void tagpu_wikistate_refuse(const char* why)
{
    if (InterlockedExchange(&s_refused, 1) == 0)
        tagpu_logf("wiki: %s - the wiki is not drawn this session", why);
}

int tagpu_wikistate_refused(void)
{
    return InterlockedCompareExchange(&s_refused, 0, 0) != 0;
}

int tagpu_wikistate_frame(unsigned frame, WIKILISTS* out)
{
    memset(out, 0, sizeof *out);
    if (!s_have || s_frame != frame) return 0;
    out->page = tagpu_huddraw_list(&s_ops);
    out->stage.watch = out->over.watch = out->top.watch = -1;
    if (s_haveStage) {
        out->stage = stage_list();
        out->stage.watch = s_curList == 1 ? s_curAt : -1;
        if (s_nover) {
            out->over = out->stage;
            out->over.op = s_over;
            out->over.n = s_nover;
            out->over.watch = s_curList == 2 ? s_curAt : -1;
        }
        if (s_ntop) {
            out->top = out->stage;
            out->top.op = s_top;
            out->top.n = s_ntop;
            out->top.watch = s_curList == 3 ? s_curAt : -1;
        }
    }
    out->hud = s_hudOn ? &s_hops : NULL;
    out->px = s_px;
    out->ox = s_ox;
    out->oy = s_oy;
    out->hpx = s_hpx;
    out->hox = s_hox;
    out->hoy = s_hoy;
    out->open = s_open;
    return 1;
}

/* a hit edge: the device pixel the draw puts the same edge on, from the screen's corner at `o` */
static int16_t dev(int o, double snapped)
{
    double r = o + snapped;
    return (int16_t)(r < -32768.0 ? -32768 : r > 32767.0 ? 32767 : r);
}

/* the hit map's room: every page control, every stage HUD control and the question's four */
_Static_assert(WIKI_MAXHOT == 160 && HUD_MAXHOT == 128, "TAGPU_CMD_WIKIHITS counts the page's and the HUD's controls");
_Static_assert(TAGPU_CMD_WIKIHITS == WIKI_MAXHOT + HUD_MAXHOT + 4, "the hit map holds every control drawn");

/* one hit, in device pixels on the attachment; 0 when the map is full */
static int hit_add(int16_t x0, int16_t y0, int16_t x1, int16_t y1, const char* key, int src, int* cut)
{
    TAGPU_CMD_WIKIHIT* t;
    if (s_map.n >= TAGPU_CMD_WIKIHITS) { (*cut)++; return 0; }
    t = &s_map.hit[s_map.n++];
    t->x0 = x0; t->y0 = y0; t->x1 = x1; t->y1 = y1;
    t->src = (uint8_t)src;
    lstrcpynA(t->key, key, sizeof t->key);
    return 1;
}

/* the page's controls of its top layer (`top` 1: About, the drawer) or of its base layer, last first */
static void page_hits(int top, int* cut)
{
    int i;
    for (i = s_ops.n - 1; i >= 0; i--) {
        const HUDOP* p = &s_ops.op[i];
        if (p->kind != HOP_HOT || ((p->flags & HOPF_TOP) != 0) != top) continue;
        hit_add(dev(s_ox, tagpu_hud_snap(p->r.x * s_px)), dev(s_oy, tagpu_hud_snap(p->r.y * s_px)),
                dev(s_ox, tagpu_hud_edge(p->r.x, p->r.w, s_px)), dev(s_oy, tagpu_hud_edge(p->r.y, p->r.h, s_px)),
                s_ops.text + p->run, WIKIHIT_PAGE, cut);
    }
}

void tagpu_wikistate_drawn(int drew, int cursor)
{
    int i, top, cut = 0;
    memset(&s_map, 0, offsetof(TAGPU_CMD_WIKI, hit));
    s_map.n = 0;
    s_drew = s_stageCursor = 0;
    if (!drew || !s_have) return;
    s_stageCursor = cursor;
    /* THE HIT ORDER, topmost first, because what is drawn over a thing takes its click: the page's
       top layer (About and the drawer lie over the stage); Leave's question while it is open, its
       frame round it counting as No; the stage HUD's controls, its top layer then its base, each
       box cut to the stage's canvas; then the page's base layer, under all of them. Within each,
       the build paints in order, so the last control painted is on top. */
    page_hits(1, &cut);
    if (s_frameOn && s_hudOn && s_rec.question) {
        static const char* const KEY[4] = { "yes", "no", "dialog", "outside" };
        const WIKIFRAMEBOX* b[4] = { &s_dlg.yes, &s_dlg.no, &s_dlg.panel, &s_dlg.frame };
        for (i = 0; i < 4; i++)
            hit_add(dev(s_dlgX, b[i]->x), dev(s_dlgY, b[i]->y), dev(s_dlgX, b[i]->x + b[i]->w), dev(s_dlgY, b[i]->y + b[i]->h),
                    KEY[i], WIKIHIT_DIALOG, &cut);
    }
    if (s_frameOn && s_hudOn)
        for (top = 1; top >= 0; top--)
            for (i = s_hops.n - 1; i >= 0; i--) {
                const HUDOP* p = &s_hops.op[i];
                int x0, y0, x1, y1;
                if (p->kind != HOP_HOT || ((p->flags & HOPF_TOP) != 0) != top) continue;
                x0 = dev(s_hox, tagpu_hud_snap(p->r.x * s_hpx));
                y0 = dev(s_hoy, tagpu_hud_snap(p->r.y * s_hpx));
                x1 = dev(s_hox, tagpu_hud_edge(p->r.x, p->r.w, s_hpx));
                y1 = dev(s_hoy, tagpu_hud_edge(p->r.y, p->r.h, s_hpx));
                if (x0 < s_cv[0]) x0 = s_cv[0];
                if (y0 < s_cv[1]) y0 = s_cv[1];
                if (x1 > s_cv[2]) x1 = s_cv[2];
                if (y1 > s_cv[3]) y1 = s_cv[3];
                if (x1 <= x0 || y1 <= y0) continue;
                hit_add((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, s_hops.text + p->run, WIKIHIT_HUD, &cut);
            }
    page_hits(0, &cut);
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
    s_map.view = (uint8_t)(s_haveStage && s_viewOk);
    s_map.side_h = (int16_t)s_viewSide;
    /* a game frame: its world takes a click only on a live frame in Try it, through the frame's fit
       (tagpu_wiki.c live_world), and the stage HUD's playhead run as drawn turns a pointer into a
       tick (TAGPU_CMD_WIKI hud_x0) */
    s_map.frame = (uint8_t)(s_haveStage && s_viewOk && s_view.frame);
    if (s_frameOn && s_hudOn && s_hops.track_x1 > s_hops.track_x0) {
        s_map.hud_x0 = dev(s_hox, tagpu_hud_snap(s_hops.track_x0 * s_hpx));
        s_map.hud_x1 = dev(s_hox, tagpu_hud_snap(s_hops.track_x1 * s_hpx));
        s_map.hud_end = s_rec.end;
        /* a live strip's run stands for [first, max(G, first)], the bounded record's; the game
           thread reads it with the run and the serial below, all of this one frame */
        if (s_rec.live) {
            s_map.hud_first = s_rec.first;
            s_map.hud_G = s_rec.G > s_rec.first ? s_rec.G : s_rec.first;
        }
    }
    /* the replay this map's HUD, run and end are of: the serial of the packet they were drawn from */
    s_map.rep_serial = s_repSerial;
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
