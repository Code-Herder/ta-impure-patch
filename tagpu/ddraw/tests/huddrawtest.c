/* huddrawtest.c -- the self-test of tagpu_huddraw.c's engine-free instance building, for
   tests/huddrawtest.py: what a HOP_TRI op becomes, and that the two chart slots keep their images,
   bands and generations apart. The shaders run only on a device, so what is checked here is the
   CPU half -- the instances and the images they name -- not the coverage the fragment stage gives.
   One line a case, "ok" or "FAILED"; exit 0 when nothing failed. */
/* stb_truetype's one implementation, which tagpu_hudart.c holds in the DLL and this test does not
   link */
#define STB_TRUETYPE_IMPLEMENTATION
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_huddraw.h"

static int s_failed;

static void said(int ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok    " : "FAILED", what);
    if (!ok) s_failed++;
}

static HUDART   s_art;                      /* no op here reads the art: no part, text or dim   */
static HUDINST  s_inst[4096];
static HUDBATCH s_b[256];
static HUDLAYREG s_g[HUD_MAXGRP];

/* `ops[0, n)` built on a 1000 x 800 attachment at 2 device pixels a unit, the screen's corner at
   (10, 20); the instance count, and the batches in `*nb` */
static int build(const HUDOP* ops, int n, const HUDCHART* chart, int slot, int* nb)
{
    HUDOPLIST l;
    HUDSHELF sh;
    int nmain, nminst, ng = 0, dropped, unfit;
    memset(&l, 0, sizeof l);
    memset(&sh, 0, sizeof sh);
    l.op = ops;
    l.n = n;
    l.chart = chart;
    l.chart_slot = slot;
    l.watch = -1;
    return tagpu_huddraw_build_list(&s_art, &l, 0, 2.0f, 10, 20, 1000, 800, s_inst, 4096, s_b, 256, nb, &nmain,
                                    &nminst, s_g, &ng, &sh, &dropped, &unfit, NULL);
}

static HUDOP tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    HUDOP o;
    memset(&o, 0, sizeof o);
    o.kind = HOP_TRI;
    o.r.x = x0; o.r.y = y0; o.r.w = x1 - x0; o.r.h = y1 - y0;
    o.shade = x2; o.size = y2;
    o.clip.w = -1;
    o.rgba = 0x336699FFu;
    return o;
}

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void tri_cases(void)
{
    HUDOP o[2];
    const HUDINST* q = &s_inst[0];
    int n, nb;
    /* (5, 7), (15, 7), (8, 30) units: (20, 34), (40, 34), (26, 80) device pixels with the corner */
    o[0] = tri(5, 7, 15, 7, 8, 30);
    n = build(o, 1, NULL, 0, &nb);
    said(n == 1 && nb == 1 && q->prm[0] == (float)HUDM_TRI, "tri: one fill instance in the triangle's mode");
    said(near(q->px[0], 20) && near(q->px[1], 34) && near(q->px[2], 40) && near(q->px[3], 34),
         "tri: px holds the first two points on the attachment");
    said(near(q->uv[0], 6) && near(q->uv[2], 6) && near(q->uv[1], 46) && near(q->uv[3], 46),
         "tri: uv holds the third point less the first, in both corners");
    said(near((q->ndc[0] + 1) * 500, 19) && near((q->ndc[2] + 1) * 500, 41) && near((q->ndc[1] + 1) * 400, 33)
             && near((q->ndc[3] + 1) * 400, 81),
         "tri: the quad is the three points' box grown by one pixel");
    said(near(q->rgba[0], 0x33 / 255.0f) && near(q->rgba[1], 0x66 / 255.0f) && near(q->rgba[2], 0x99 / 255.0f)
             && near(q->rgba[3], 1),
         "tri: its colour, premultiplied");
    /* the third point as a point, not an offset: a triangle whose third point lies left of and
       above the first */
    o[0] = tri(50, 60, 70, 65, 40, 10);
    n = build(o, 1, NULL, 0, &nb);
    said(n == 1 && near(q->uv[0], -20) && near(q->uv[1], -100) && near((q->ndc[0] + 1) * 500, 89)
             && near((q->ndc[1] + 1) * 400, 39),
         "tri: a third point above and left of the first bounds the box");
    o[0] = tri(0, 0, 10, 10, 20, 20);
    o[1] = tri(0, 0, 10, 0, NAN, 4);
    said(build(o, 2, NULL, 0, &nb) == 0, "tri: three points on a line, and a point that is not finite, draw nothing");
    o[0] = tri(0, 0, 1e20f, 0, 0, 1e20f);
    said(build(o, 1, NULL, 0, &nb) == 0, "tri: finite points whose area is past float's range draw nothing");
}

static void chart_data(HUDCHART* k, float y)
{
    int j;
    memset(k, 0, sizeof *k);
    k->n = 1;
    k->npt = 12;
    k->w = 2;
    k->halo_w = 4;
    k->cross = 3;
    k->cross_w = 1;
    k->g[0].rgb = 0xFF0000;
    for (j = 0; j < k->npt; j++) { k->pt[0][j][0] = 100 + 10.0f * j; k->pt[0][j][1] = y + (float)(j % 3); }
}

static int all_tex(int nb, int tex)
{
    int i;
    for (i = 0; i < nb; i++)
        if (s_b[i].tex != tex) return 0;
    return nb > 0;
}

static void chart_cases(void)
{
    static HUDCHART a, b;
    static float keep0[HUD_CHART_H][HUD_CHART_W][4];
    HUDOP o;
    unsigned g0, g1, h0, h1, x;
    int n, nb;
    memset(&o, 0, sizeof o);
    o.kind = HOP_CHART;
    o.r.w = 400; o.r.h = 200;
    o.clip.w = -1;
    o.rgba = 0xFFFFFFFFu;
    chart_data(&a, 50);
    chart_data(&b, 120);

    tagpu_huddraw_chart(0, &g0);
    tagpu_huddraw_chart(1, &g1);
    n = build(&o, 1, &a, 0, &nb);
    tagpu_huddraw_chart(0, &h0);
    tagpu_huddraw_chart(1, &h1);
    said(n > 0 && all_tex(nb, HUD_TEX_CHART) && h0 != g0 && h1 == g1,
         "chart: slot 0's bands name HUD_TEX_CHART and move slot 0's generation alone");
    memcpy(keep0, tagpu_huddraw_chart(0, &x), sizeof keep0);

    n = build(&o, 1, &b, 1, &nb);
    tagpu_huddraw_chart(0, &g0);
    tagpu_huddraw_chart(1, &g1);
    said(n > 0 && all_tex(nb, HUD_TEX_CHART_WIKI) && g0 == h0 && g1 != h1,
         "chart: slot 1's bands name HUD_TEX_CHART_WIKI and move slot 1's generation alone");
    said(!memcmp(keep0, tagpu_huddraw_chart(0, &x), sizeof keep0),
         "chart: building slot 1 leaves slot 0's image as it was");
    said(memcmp(keep0, tagpu_huddraw_chart(1, &x), sizeof keep0) != 0, "chart: slot 1's image is its own chart's");

    n = build(&o, 1, &a, 0, &nb);
    tagpu_huddraw_chart(0, &h0);
    tagpu_huddraw_chart(1, &h1);
    said(n > 0 && h0 == g0 && h1 == g1, "chart: slot 0 again with the same lines moves no generation");

    said(build(&o, 1, &a, 2, &nb) == 0 && build(&o, 1, &a, -1, &nb) == 0, "chart: a slot that is not one draws nothing");
    said(tagpu_huddraw_chart(2, &x) == NULL && x == 0 && tagpu_huddraw_chart(-1, &x) == NULL && x == 0,
         "chart: a slot that is not one has no image and generation 0");
}

int main(void)
{
    tri_cases();
    chart_cases();
    printf("huddrawtest: %s\n", s_failed ? "FAILED" : "all ok");
    return s_failed ? 1 : 0;
}
