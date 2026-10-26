/* tagpu_wikistate.c -- the in-game wiki's render-thread half. Contract: tagpu_wikistate.h. */
#include <windows.h>
#include <string.h>
#include "tagpu.h"
#include "tagpu_wikistate.h"
#include "tagpu_wikiframe.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudart.h"
#include "tagpu_hud.h"
#include "tagpu_log.h"

static HUDOPS   s_ops;                  /* this frame's; 400 KB, never on a stack              */
static int      s_have;
static unsigned s_frame;                /* the render-thread frame s_ops is for                 */
static float    s_px;
static int      s_ox, s_oy;
static int      s_open, s_ptr;          /* the record's: the window open, a pointer in the picture */
static volatile LONG s_refused;         /* the layout refused the kit: said once, drawn never;
                                           read by the game thread (tagpu_wikistate_refused)    */
static int      s_drew;                 /* this frame's layer was drawn                         */
static TAGPU_CMD_WIKI s_map;
static int      s_mapCut;

void tagpu_wikistate_present(const TAGPU_FRAME* f)
{
    const struct TAGPU_PACKET* pk = f->packet;
    const TAGPU_PK_WIKI* w;
    const WIKIART* art;
    const HUDSTATE* hs;
    WIKIFRAME in;
    HUDRECT body;
    char err[160];
    float u, hpx;
    int vw = f->vp_w, vh = f->vp_h, pw = 128, bh = 32, q8 = 256, hox, hoy;

    s_have = 0;
    if (s_refused || !pk || !pk->in_game || !(w = tagpu_pk_wiki(pk))) return;
    /* THE ART LOADS ON ITS OWN THREAD (tagpu_hudart.c), started by whichever half asks first; the
       wiki draws from the first frame after it is published */
    if (!(art = tagpu_wikiart_get())) {
        tagpu_hudart_start();
        return;
    }
    if (vw > 0 && vh > 0) { s_ox = f->vp_x; s_oy = f->win_height - f->vp_y - vh; }
    else { vw = f->win_width; vh = f->win_height; s_ox = s_oy = 0; }
    if (f->game_width <= 0 || f->game_height <= 0 || vw <= 0 || vh <= 0 || s_oy < 0) return;

    /* THE WIKI UNIT is the screen's height / 720 device pixels, whatever the HUD scale (the
       mockup's --u); the button alone is placed in the top bar's own pixels, which the HUD scale
       sizes: the bar's height on screen over its 32 (tagpu_hud_live, as tagpu_hudstate.c reads it) */
    u = (float)vh / 720.0f;
    tagpu_hud_live(&pw, &bh, &q8);
    memset(&in, 0, sizeof in);
    in.W = (float)vw / u;
    in.H = (float)vh / u;
    in.bar = (float)bh * (float)vh / (float)f->game_height / u / 32.0f;
    /* the rewind button's slot is taken when the same frame's live HUD draws its button */
    in.rewind = tagpu_hudstate_frame(f->frame_counter, &hs, &hpx, &hox, &hoy) && hs->live;
    in.open = w->open;
    in.btn_down = w->btn_down;
    in.x_down = w->x_down;
    if (!tagpu_wikiframe_build(art->draw.lay, &in, &s_ops, &body, err, sizeof err)) {
        InterlockedExchange(&s_refused, 1);
        tagpu_logf("wiki: the shell's layout refused the kit (%s) - the wiki is not drawn this session", err);
        return;
    }
    s_open = w->open;
    s_ptr = w->ptr_x >= 0 && w->ptr_y >= 0;
    s_px = u;
    s_frame = f->frame_counter;
    s_have = 1;
}

int tagpu_wikistate_refused(void)
{
    return InterlockedCompareExchange(&s_refused, 0, 0) != 0;
}

int tagpu_wikistate_frame(unsigned frame, const HUDOPS** ops, float* px, int* ox, int* oy)
{
    if (!s_have || s_frame != frame) return 0;
    *ops = &s_ops;
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

void tagpu_wikistate_drawn(const HUDOPS* ops)
{
    int i, cut = 0;
    s_map.n = 0;
    s_map.drawn = 0;
    s_map.open = 0;
    s_drew = 0;
    if (!ops || !s_have) return;
    /* topmost first: the build paints in order, so the last control painted is on top */
    for (i = ops->n - 1; i >= 0; i--) {
        const HUDOP* p = &ops->op[i];
        TAGPU_CMD_WIKIHIT* t;
        if (p->kind != HOP_HOT) continue;
        if (s_map.n >= TAGPU_CMD_WIKIHITS) { cut++; continue; }
        t = &s_map.hit[s_map.n++];
        t->x0 = dev(s_ox, tagpu_hud_snap(p->r.x * s_px));
        t->y0 = dev(s_oy, tagpu_hud_snap(p->r.y * s_px));
        t->x1 = dev(s_ox, tagpu_hud_edge(p->r.x, p->r.w, s_px));
        t->y1 = dev(s_oy, tagpu_hud_edge(p->r.y, p->r.h, s_px));
        lstrcpynA(t->key, ops->text + p->run, sizeof t->key);
    }
    if (cut && !s_mapCut) {
        s_mapCut = 1;
        tagpu_logf("wiki: %d controls past the hit map's %u are not clickable (said once)", cut,
                   TAGPU_CMD_WIKIHITS);
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
    s_map.n = 0;
    s_map.drawn = 0;
    s_map.open = 0;
}
