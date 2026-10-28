/* tagpu_wikiside.c -- the wiki stage's sidebar column composed in the mod's pictures. Contract:
   tagpu_wikiside.h; the specification is tools/wiki/mockup/side.js, whose names and order this
   file follows. Render thread; pure: it reads its arguments and writes `out`. */
#include <limits.h>
#include <string.h>
#include "tagpu_wikiside.h"

#define COL WIKI_SIDE_W

/* What lies where nothing else is painted -- under and below a screen shorter than the column, in
   a panel picture's transparent pixels: palette index 0. The HUD pass 0x467D70 fills its whole
   surface with 0 (SurfaceFill 0x4C6890 at 0x467D87) before PANELSIDE, DISASSEMBLED; all six compat
   setups' captures show (0, 0, 0) there, index 0 of each install's palette (MEASURED 2026-10-27). */
#define CLEAR 0

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* a palette index as side.js's read() takes one: 0..255, else 0 (the loader's -1 for none) */
static int index8(int v) { return v >= 0 && v < 256 ? v : 0; }

/* A FRAME IS DATA UNTIL BOUNDED: its rect inside the sheet and its hotspot within a GAF's int16,
   so the reads below stay in the sheet and no coordinate sum overflows. The loader checks the
   rects before it publishes (tagpu_wikiasset.h); this check is what the composer itself rests on. */
static int frame_ok(const WIKISIDE* art, const WIKISIDEFRAME* f)
{
    return f->sx >= 0 && f->sy >= 0 && f->w > 0 && f->h > 0 && f->w <= art->sheet_w - f->sx &&
           f->h <= art->sheet_h - f->sy && f->hx >= -32768 && f->hx <= 32767 && f->hy >= -32768 && f->hy <= 32767;
}

/* side.js `blit`: frame `f` with its top left at (dx, dy), clipped to [x0, x1) x [y0, y1) and the
   column, skipping the key index (-1: none) */
static void blit(const WIKISIDE* art, WIKISIDECOL* o, const WIKISIDEFRAME* f, int dx, int dy, int key,
                 int cx0, int cy0, int cx1, int cy1)
{
    int x0, x1, y0, y1, x, y;
    if (!frame_ok(art, f)) return;
    x0 = imax(imax(dx, cx0), 0);
    x1 = imin(imin(dx + f->w, cx1), COL);
    y0 = imax(imax(dy, cy0), 0);
    y1 = imin(imin(dy + f->h, cy1), o->h);
    for (y = y0; y < y1; y++) {
        /* y - dy lies in [0, f->h) and x - dx in [0, f->w): inside the frame, so inside the sheet */
        const unsigned char* src = art->sheet + (size_t)(f->sy + y - dy) * (size_t)art->sheet_w + (size_t)f->sx;
        for (x = x0; x < x1; x++) {
            const int p = src[x - dx];
            if (p == key) continue;
            o->idx[y * COL + x] = (unsigned char)p;
            o->a[y * COL + x] = 1;
        }
    }
}

/* a pixel the minimap writes: side.js writes into a typed array, which drops a write past its end,
   so a row past the column is dropped here too */
static void put(WIKISIDECOL* o, int x, int y, int v)
{
    if (x < 0 || x >= COL || y < 0 || y >= o->h) return;
    o->idx[y * COL + x] = (unsigned char)v;
    o->a[y * COL + x] = 1;
}

/* the palette entry nearest (squared RGB distance, the first index on a tie) to a colour */
static int nearest(const unsigned char* pal, int r, int g, int b)
{
    int best = 0, bd = INT_MAX, j;
    for (j = 0; j < 256; j++) {
        const int dr = pal[j * 4] - r, dg = pal[j * 4 + 1] - g, db = pal[j * 4 + 2] - b, d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    return best;
}

/* side.js's minimap: the base under the fog, the dots, the camera box */
static int minimap(const WIKISIDE* art, const WIKISTAGEVIEW* v, const unsigned char* base, int nbase, WIKISIDECOL* o)
{
    WIKISIDEMAP m;
    const int W = v->w * WIKI_CELL, H = v->h * WIKI_CELL, mw = v->w, mh = v->h, cols = mw / 2, rows = mh / 2;
    const int un = index8(art->unexplored), bx = index8(art->box);
    int x, y, i, x0, y0, x1, y1;
    if (!base || mw <= 0 || mh <= 0) return 0;
    tagpu_wikiside_geometry(W, H, &m);
    /* THE BASE IS DATA UNTIL BOUNDED: exactly the picture's pixels, as side.js takes it */
    if (nbase != m.w * m.h) return 0;
    /* 0x466C20: each picture pixel takes the fog of its 32-unit cell, read at the cell's first
       16-unit cell; 2r < mh and 2c < mw since r < rows and c < cols, and the index is bounded
       against the copy's map besides */
    for (y = 0; y < m.h; y++)
        for (x = 0; x < m.w; x++) {
            const int c = x * cols / m.w, r = y * rows / m.h, fi = 2 * r * mw + 2 * c;
            const int f = fi >= 0 && fi < mw * mh ? v->fog[fi] : 0, b = base[y * m.w + x];
            put(o, m.offx + x, m.offy + y, f == 2 ? un : f == 1 ? art->seen[b] : b);
        }
    x0 = m.offx;
    y0 = m.offy;
    x1 = m.offx + m.w;
    y1 = m.offy + m.h;
    /* radlogo's frame is the owner's colour (PlayerInfo+0x96), as the 3D look's team faces take
       it; an owner past the frames has no dot */
    for (i = 0; i < v->nunit && i < WIKISTAGE_UNITS; i++) {
        const WIKISTAGEUNIT* u = &v->unit[i];
        if (u->owner < art->nradlogo && u->owner < WIKISIDE_DOTS) {
            const WIKISIDEFRAME* f = &art->radlogo[u->owner];
            int px, py;
            tagpu_wikiside_dot(u->tx, u->tz, &m, W, H, &px, &py);
            blit(art, o, f, m.offx + px - f->hx, m.offy + py - f->hy, art->dot_key, x0, y0, x1, y1);
        }
    }
    /* the unit under the pointer gets radlogohigh as well (0x466DC0, +0xA8 == main+0x2CBA) */
    if (art->has_high && v->side.under >= 0 && v->side.under < v->nunit && v->side.under < WIKISTAGE_UNITS) {
        const WIKISTAGEUNIT* u = &v->unit[v->side.under];
        int px, py;
        tagpu_wikiside_dot(u->tx, u->tz, &m, W, H, &px, &py);
        blit(art, o, &art->high, m.offx + px - art->high.hx, m.offy + py - art->high.hy, art->dot_key, x0, y0, x1, y1);
    }
    /* 0x466B70: the camera box. The stage shows the whole map, so the box is the picture's border */
    for (x = x0; x < x1; x++) {
        put(o, x, y0, bx);
        put(o, x, y1 - 1, bx);
    }
    for (y = y0; y < y1; y++) {
        put(o, x0, y, bx);
        put(o, x1 - 1, y, bx);
    }
    return 1;
}

int tagpu_wikiside_compose(const WIKISIDE* art, const WIKISTAGEVIEW* v, const unsigned char* base, int nbase,
                           WIKISIDECOL* out)
{
    const WIKILAYOUT* L;
    int H, i;
    out->h = 0;
    out->minimap = 0;
    if (!art || !v || !v->has_side || !art->sheet || art->sheet_w <= 0 || art->sheet_w > WIKISIDE_SHEET_W ||
        art->sheet_h <= 0 || art->sheet_h > WIKISIDE_SHEET_MAXH)
        return 0;
    L = &art->layout;
    H = v->side.h;
    if (v->layout != L || H < 1 || H > WIKI_SIDE_ROWS) return 0;
    out->h = H;
    memset(out->idx, CLEAR, (size_t)COL * (size_t)H);
    memset(out->a, 1, (size_t)COL * (size_t)H);

    /* PANELSIDE lands with its top left at (0, 0) whatever its hotspot: 0x467D70 passes the
       frame's own hotspot as the point (0x467E30..0x467E3C) and CopyGafToContext 0x4B7F90
       subtracts it again, DISASSEMBLED */
    if (art->has_panelside) blit(art, out, &art->panelside, 0, 0, art->panelside_key, 0, 0, COL, H);

    out->minimap = minimap(art, v, base, nbase, out);

    /* THE SCREEN IS DATA UNTIL BOUNDED: an index of the checked layout, whose screens and gadget
       rects lie inside the column (tagpu_wikilayout_check); the screen's art is parallel to it */
    if (v->side.screen >= 0 && v->side.screen < L->nscreen && v->side.screen < WIKI_SIDE_SCREENS) {
        const WIKISCREEN* scr = &L->screen[v->side.screen];
        const WIKISIDESCREEN* sa = &art->screen[v->side.screen];
        const int y0 = scr->y, n = imin(imin(scr->ngadget, v->side.nstate), WIKI_SIDE_GADGETS);
        /* THE PANEL PICTURE (0x4B0230): drawn once at the panel's origin minus its hotspot, not
           tiled. With no picture the engine fills the panel with GUI colour gi+0x8C6, which the
           art does not carry: INTERIM, the palette entry nearest the whitebox panel's colour,
           #3a3a36 (tagpu_wikistage.c SIDE_WB). A panel frame outside the sheet is no picture, as
           side.js's read() refuses one */
        if (sa->has_panel && frame_ok(art, &sa->panel))
            blit(art, out, &sa->panel, -sa->panel.hx, y0 - sa->panel.hy, sa->panel_key, 0, y0, scr->w, y0 + scr->h);
        else {
            const int fill = nearest(art->pal, 0x3a, 0x3a, 0x36);
            int x, y;
            for (y = imax(y0, 0); y < imin(y0 + scr->h, H); y++)
                for (x = 0; x < imin(scr->w, COL); x++) {
                    out->idx[y * COL + x] = (unsigned char)fill;
                    out->a[y * COL + x] = 1;
                }
        }
        /* THE BUTTONS: GUI_ButtonDraw 0x4A5F40 passes the gadget's corner PLUS the hotspot, so a
           button's frame lands with its top left on the gadget's corner whatever its hotspot.
           A label picture paints nothing: the stage draws it with ops over the column. */
        for (i = 0; i < n; i++) {
            const WIKIGADGET* g = &scr->gadget[i];
            const WIKISIDEPIC* p = &sa->pic[i];
            const int st = v->side.state[i], nf = imin(p->nframe, 3);
            const WIKISIDEFRAME* f;
            if (st == -1 || p->from == WIKISIDE_FROM_LABEL || nf <= 0) continue;
            /* frame(state): 0 at rest; 1 pressed or the current tab, which a one-frame sequence
               does not have -- 0x4B7F30 returns none past the count and 0x4A6176 skips the blit,
               so a held one-frame button shows the panel under it (0x4A6110..0x4A614B);
               greyed, the last frame for attribs 0x100, else min(2, n - 1) */
            if (st == 0) f = &p->frame[0];
            else if (st == 1) f = nf > 1 ? &p->frame[1] : NULL;
            else f = (p->attribs & 0x100) ? &p->frame[nf - 1] : &p->frame[imin(2, nf - 1)];
            if (f) blit(art, out, f, g->x, y0 + g->y, p->key, 0, 0, COL, H);
            /* GREYED, the engine tints the gadget's whole rect once its frame is drawn, what lies
               under the frame's transparent pixels included (0x4A699B..0x4A69BA, DISASSEMBLED):
               the grey remap, then the box shader at level -20, composed into one table. Not for
               attribs 0x100, whose last frame is the greyed picture, nor for a checkbox (attribs
               0x80), as GUI_ButtonDraw decides at 0x4A607E..0x4A6106. Without the table the
               frame stands untinted. */
            if (st == 2 && art->has_greyed && !(p->attribs & 0x180)) {
                int x, y;
                for (y = imax(y0 + g->y, 0); y < imin(y0 + g->y + g->h, H); y++)
                    for (x = imax(g->x, 0); x < imin(g->x + g->w, COL); x++)
                        out->idx[y * COL + x] = art->greyed[out->idx[y * COL + x]];
            }
        }
    }
    return H;
}

void tagpu_wikiside_rgba(const WIKISIDE* art, const WIKISIDECOL* c, unsigned char* out)
{
    const int n = COL * (c->h > 0 && c->h <= WIKI_SIDE_ROWS ? c->h : 0);
    int i;
    for (i = 0; i < n; i++) {
        unsigned char* q = out + (size_t)i * 4;
        if (c->a[i]) {
            const unsigned char* p = art->pal + c->idx[i] * 4;
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = 255;
        } else
            q[0] = q[1] = q[2] = q[3] = 0;
    }
}
