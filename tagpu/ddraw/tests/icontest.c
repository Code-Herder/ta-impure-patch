/* icontest.c -- the strategic icons' rule and shapes (inc/tagpu_icon_rule.h),
   run on the host by tests/icontest.py, or under Wine as `make icontest`.

     1. the defaults' sizes and levels at 1920x1080, 2560x1440 and 3840x2160
        against the plan's table (research/notes/mega-map-plan.md, landing 3),
        to +-0.001;
     2. the fade ramp's two ends, and a zero band as a step;
     3. at 1x with the defaults, no icon -- alpha 0 for every class and the
        radar dot -- on every common display shape up to 4K native: the
        promise that 1x stays the stock frame;
     4. the drawn edge's floor: every class's edge is TAGPU_ICON_EDGE_MIN or
        more at every icon scale on every frame, and BAR's size where the
        floor does not bind; the switch follows BAR's size and the cover
        test the drawn edge where it does;
     5. each shape rasterised at every edge from the floor to 128 px:
        symmetric about x = 0, ink on its border, no hole in a solid shape,
        an open centre in a hollow one, from 16 px the plus at the centre of
        the builder classes alone and fill at the centre of the other solid
        ones, the nine pairwise different -- and at one px under the floor
        some two the same, which is why the floor is where it is -- and the
        16-px rasters' FNV against a golden value.

   Under the floor the 1-px ink alone decides the smaller shapes: at 6 px
   COMMANDER is COMBAT, at 4 px the three triangles are one outline too. Both
   hollow centres are open from 5 px, under the floor.

   `icontest dump` prints the 16-px rasters. Exit 0 when nothing failed. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tagpu_icon_rule.h"

static int s_fail, s_checks;

#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const char* const NAME[TAGPU_ICLS_N] = {
    "UNKNOWN", "COMMANDER", "FACTORY", "BUILDING", "AIRCONS", "AIRCOMBAT", "AIR", "CONS", "COMBAT" };

/* the level at which d' = d for an icon of `edge` px */
static float level_at(const TAGPU_ICON_VIEW* v, int edge, float d)
{
    return v->zS * v->base / ((float)edge * d);
}

/* ---- 1. the defaults' table ---- */
typedef struct { int cls, edge; float fade, opaque; } ROW;

static void table(int gw, int gh, float base, float zS, const ROW* rows, int nrows)
{
    TAGPU_ICON_VIEW v;
    int i;
    tagpu_icon_view(&v, gw, gh, 100, 100, 33);
    CHECK(fabsf(v.base - base) < 0.001f, "%dx%d base %.4f, want %.4f", gw, gh, v.base, base);
    CHECK(fabsf(v.zS - zS) < 0.001f, "%dx%d zS %.4f, want %.4f", gw, gh, v.zS, zS);
    for (i = 0; i < nrows; i++) {
        const ROW* r = &rows[i];
        const int e = r->cls < 0 ? 4 * tagpu_icon_dot_k(&v) : tagpu_icon_gen_edge(&v, r->cls);
        const char* nm = r->cls < 0 ? "radar dot" : NAME[r->cls];
        CHECK(e == r->edge, "%dx%d %s edge %d, want %d", gw, gh, nm, e, r->edge);
        if (r->fade > 0.0f) {
            const float f = level_at(&v, e, 1.0f - v.band), o = level_at(&v, e, 1.0f);
            CHECK(fabsf(f - r->fade) <= 0.001f, "%dx%d %s fades in below %.4f, want %.3f", gw, gh, nm, f, r->fade);
            CHECK(fabsf(o - r->opaque) <= 0.001f, "%dx%d %s opaque below %.4f, want %.3f", gw, gh, nm, o, r->opaque);
        }
    }
}

static void test_table(void)
{
    static const ROW R1080[] = {
        { TAGPU_ICLS_COMBAT, 8, 0.409f, 0.274f }, { -1, 8, 0.409f, 0.274f },
        { TAGPU_ICLS_AIRCOMBAT, 9, 0.363f, 0.243f }, { TAGPU_ICLS_AIR, 9, 0.363f, 0.243f },
        { TAGPU_ICLS_CONS, 10, 0.327f, 0.219f }, { TAGPU_ICLS_BUILDING, 10, 0.327f, 0.219f },
        { TAGPU_ICLS_UNKNOWN, 10, 0.327f, 0.219f }, { TAGPU_ICLS_AIRCONS, 12, 0.273f, 0.183f },
        { TAGPU_ICLS_FACTORY, 14, 0.233f, 0.156f }, { TAGPU_ICLS_COMMANDER, 16, 0.204f, 0.137f } };
    /* 4K native. BUILDING is 21 px here, not CONS's 20 (base 20.16 x 1.0375 =
       20.92), so it has levels of its own */
    static const ROW R2160[] = {
        { TAGPU_ICLS_COMBAT, 16, 0.817f, 0.547f }, { -1, 16, 0.817f, 0.547f },
        { TAGPU_ICLS_AIRCOMBAT, 18, 0.726f, 0.487f }, { TAGPU_ICLS_AIR, 18, 0.726f, 0.487f },
        { TAGPU_ICLS_CONS, 20, 0.654f, 0.438f }, { TAGPU_ICLS_UNKNOWN, 20, 0.654f, 0.438f },
        { TAGPU_ICLS_BUILDING, 21, 0.623f, 0.417f }, { TAGPU_ICLS_AIRCONS, 24, 0.545f, 0.365f },
        { TAGPU_ICLS_FACTORY, 28, 0.467f, 0.313f }, { TAGPU_ICLS_COMMANDER, 33, 0.396f, 0.266f } };
    /* 4K at HUD scale 150: the engine's frame is 2560x1440 */
    static const ROW R1440[] = { { TAGPU_ICLS_COMBAT, 11, 0.0f, 0.0f } };
    table(1920, 1080, 10.08f, 0.2173f, R1080, (int)(sizeof R1080 / sizeof R1080[0]));
    table(3840, 2160, 20.16f, 0.4346f, R2160, (int)(sizeof R2160 / sizeof R2160[0]));
    table(2560, 1440, 13.44f, 0.2897f, R1440, 1);
}

/* ---- 2. the ramp ---- */
static void test_ramp(void)
{
    static const int FADE[] = { 33, 67 };
    TAGPU_ICON_VIEW v;
    int i, hide, a;
    for (i = 0; i < 2; i++) {
        tagpu_icon_view(&v, 1920, 1080, 100, 100, FADE[i]);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - v.band + 1e-4f), 10, 10, 0, &hide);
        CHECK(a == 64, "fade %d: alpha %d at the band's start, want 64", FADE[i], a);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - v.band - 1e-4f), 10, 10, 0, &hide);
        CHECK(a == 0, "fade %d: alpha %d before the band, want 0", FADE[i], a);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - 1e-4f), 10, 10, 0, &hide);
        CHECK(a == 255 && !hide, "fade %d: alpha %d hide %d at the band's end, want 255, 0", FADE[i], a, hide);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f + 1e-3f), 10, 10, 0, &hide);
        CHECK(a == 255 && hide, "fade %d: alpha %d hide %d past the band, want 255, 1", FADE[i], a, hide);
        /* a model wider than 0.45 of the icon is never hidden */
        a = tagpu_icon_eval(&v, 0.05f, 10, 10, 1000, &hide);
        CHECK(a == 255 && !hide, "fade %d: a large model hidden at 0.05", FADE[i]);
    }
    tagpu_icon_view(&v, 1920, 1080, 100, 100, 0);
    a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - 1e-4f), 10, 10, 0, &hide);
    CHECK(a == 0, "fade 0: alpha %d just before the switch, want 0", a);
    a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f + 1e-4f), 10, 10, 0, &hide);
    CHECK(a == 255, "fade 0: alpha %d just past the switch, want 255", a);
}

/* ---- 3. 1x is the stock frame ---- */
static void test_one(void)
{
    /* every common display shape, from 4:3 to 32:9, up to 4K native's height */
    static const int ASPECT[][2] = { { 4, 3 }, { 5, 4 }, { 16, 10 }, { 16, 9 }, { 21, 9 }, { 32, 9 } };
    int a, gh, c, hide;
    for (a = 0; a < (int)(sizeof ASPECT / sizeof ASPECT[0]); a++)
        for (gh = 480; gh <= 2160; gh++) {
            const int gw = gh * ASPECT[a][0] / ASPECT[a][1];
            TAGPU_ICON_VIEW v;
            tagpu_icon_view(&v, gw, gh, 100, 100, 33);
            for (c = -1; c < TAGPU_ICLS_N; c++) {
                const int e = c < 0 ? 4 * tagpu_icon_dot_k(&v) : tagpu_icon_gen_edge(&v, c);
                const int al = tagpu_icon_eval(&v, 1.0f, c < 0 ? e : tagpu_icon_bar_edge(&v, c), e, 0, &hide);
                CHECK(al == 0 && !hide, "%dx%d at 1x: %s alpha %d", gw, gh, c < 0 ? "radar dot" : NAME[c], al);
            }
        }
}

/* ---- 4. the floor ---- */
static void test_floor(void)
{
    static const int SCALE[] = { 75, 100, 125, 150, 200 };
    /* the longest side, by scale, up to which the floor binds on some class */
    static const int BINDS[] = { 2060, 1545, 1236, 1030, 772 };
    TAGPU_ICON_VIEW v;
    int s, m, c, a, hide, size, edge;
    for (s = 0; s < (int)(sizeof SCALE / sizeof SCALE[0]); s++) {
        int last = 0;
        for (m = 1; m <= 7680; m++) {
            tagpu_icon_view(&v, m, m * 9 / 16, SCALE[s], 100, 33);
            for (c = 0; c < TAGPU_ICLS_N; c++) {
                const int b = tagpu_icon_bar_edge(&v, c), e = tagpu_icon_gen_edge(&v, c);
                CHECK(e >= TAGPU_ICON_EDGE_MIN, "scale %d, %d px: %s drawn at %d px", SCALE[s], m, NAME[c], e);
                CHECK(e == (b > TAGPU_ICON_EDGE_MIN ? b : TAGPU_ICON_EDGE_MIN),
                      "scale %d, %d px: %s drawn at %d px, BAR's size %d", SCALE[s], m, NAME[c], e, b);
                if (b != e) last = m;
            }
        }
        CHECK(last == BINDS[s], "scale %d: the floor binds up to %d px, want %d", SCALE[s], last, BINDS[s]);
    }
    /* scale 75 at 1920x1080: COMBAT is 6 px by BAR's rule and drawn at 7. The
       switch is at BAR's 6, and a model wider than 0.45 x 6 but under
       0.45 x 7 at that level is hidden: the drawn icon covers it */
    tagpu_icon_view(&v, 1920, 1080, 75, 100, 33);
    size = tagpu_icon_bar_edge(&v, TAGPU_ICLS_COMBAT);
    edge = tagpu_icon_gen_edge(&v, TAGPU_ICLS_COMBAT);
    CHECK(size == 6 && edge == 7, "scale 75, 1920x1080: COMBAT size %d edge %d, want 6, 7", size, edge);
    a = tagpu_icon_eval(&v, level_at(&v, size, 1.0f - v.band - 1e-4f), size, edge, 0, &hide);
    CHECK(a == 0, "the floored COMBAT: alpha %d before BAR's band, want 0", a);
    a = tagpu_icon_eval(&v, level_at(&v, size, 1.0f - v.band + 1e-4f), size, edge, 0, &hide);
    CHECK(a == 64, "the floored COMBAT: alpha %d at BAR's band's start, want 64", a);
    {
        const float z = level_at(&v, size, 1.0f + 1e-3f);
        const unsigned r = (unsigned)(0.45f * 6.5f / z);
        CHECK((float)r * z > 0.45f * 6.0f && (float)r * z < 0.45f * 7.0f, "the cover radius %u at %.4f", r, z);
        a = tagpu_icon_eval(&v, z, size, edge, r, &hide);
        CHECK(a == 255 && hide, "the floored COMBAT: alpha %d hide %d over a model the drawn icon covers", a, hide);
    }
}

/* ---- 5. the shapes ---- */
static unsigned char s_ras[128 * 128];

static void raster(int cls, int e)
{
    const float fw = 2.0f / (float)e;
    int i, j;
    for (j = 0; j < e; j++)
        for (i = 0; i < e; i++)
            /* the pixel centre, ((2i + 1) - e) / e: an exact negation at i and
               e - 1 - i, so the symmetry below is the shape's and not the
               arithmetic's */
            s_ras[j * e + i] = (unsigned char)tagpu_icon_shape(cls, (float)(2 * i + 1 - e) / (float)e,
                                                               (float)(2 * j + 1 - e) / (float)e, fw, fw);
}

/* the unlit texels joined to the quad's edge, by a flood from it */
static unsigned char s_out[128 * 128];
static int s_stack[128 * 128];

static void outside(int e)
{
    int n = 0, i;
    memset(s_out, 0, (size_t)e * e);
    for (i = 0; i < e * e; i++) {
        const int x = i % e, y = i / e;
        if ((x == 0 || y == 0 || x == e - 1 || y == e - 1) && !s_ras[i] && !s_out[i]) {
            s_out[i] = 1; s_stack[n++] = i;
        }
    }
    while (n) {
        const int t = s_stack[--n], x = t % e, y = t / e;
        const int nb[4] = { x > 0 ? t - 1 : -1, x < e - 1 ? t + 1 : -1, y > 0 ? t - e : -1, y < e - 1 ? t + e : -1 };
        int k;
        for (k = 0; k < 4; k++)
            if (nb[k] >= 0 && !s_ras[nb[k]] && !s_out[nb[k]]) { s_out[nb[k]] = 1; s_stack[n++] = nb[k]; }
    }
}

static unsigned fnv(const unsigned char* p, int n, unsigned h)
{
    int i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

#define GOLDEN16 0x4EC8F4CDu

static void test_shapes(int dump)
{
    static unsigned char ras[TAGPU_ICLS_N][128 * 128];
    int c, e, i, j, k;
    unsigned h = 2166136261u;
    for (e = TAGPU_ICON_EDGE_MIN - 1; e <= 128; e++) {
        for (c = 0; c < TAGPU_ICLS_N; c++) {
            int asym = 0, edgeInk = 1, any = 0, open = 0;
            const int sil = tagpu_icon_cls_sil(c);
            raster(c, e);
            for (j = 0; j < e; j++)
                for (i = 0; i < e; i++) {
                    if (s_ras[j * e + i] != s_ras[j * e + (e - 1 - i)]) asym++;
                    if (s_ras[j * e + i]) any = 1;
                    /* an unlit texel inside the silhouette: an open centre */
                    if (!s_ras[j * e + i] &&
                        tagpu_icon_sil(sil, (float)(2 * i + 1 - e) / (float)e, (float)(2 * j + 1 - e) / (float)e))
                        open = 1;
                }
            /* every lit texel next to the outside -- the unlit texels joined to
               the quad's edge, which leaves a hollow shape's centre out -- or
               on the quad's edge is ink */
            outside(e);
            for (j = 0; j < e; j++)
                for (i = 0; i < e; i++) {
                    int out;
                    if (!s_ras[j * e + i]) continue;
                    out = i == 0 || i == e - 1 || j == 0 || j == e - 1 ||
                          s_out[j * e + i - 1] || s_out[j * e + i + 1] ||
                          s_out[(j - 1) * e + i] || s_out[(j + 1) * e + i];
                    if (out && s_ras[j * e + i] != 2) edgeInk = 0;
                }
            memcpy(ras[c], s_ras, (size_t)e * e);
            if (e < TAGPU_ICON_EDGE_MIN) continue;
            CHECK(any, "%s at %d px: nothing lit", NAME[c], e);
            CHECK(!asym, "%s at %d px: %d texels differ from their mirror", NAME[c], e, asym);
            CHECK(edgeInk, "%s at %d px: a lit texel on the border is not ink", NAME[c], e);
            if (tagpu_icon_cls_hollow(c)) {
                CHECK(open, "%s at %d px: the hollow centre is closed", NAME[c], e);
            } else {
                CHECK(!open, "%s at %d px: a solid shape has a hole", NAME[c], e);
                if (e >= 16) {
                    /* the centre texels: the plus on a builder, fill otherwise */
                    const int m = s_ras[(e / 2) * e + e / 2];
                    const int want = tagpu_icon_cls_plus(c) ? 2 : 1;
                    CHECK(m == want, "%s at %d px: centre %d, want %d", NAME[c], e, m, want);
                }
            }
            if (e == 16) {
                h = fnv(s_ras, 256, h);
                if (dump) {
                    printf("%s, 16 px:\n", NAME[c]);
                    for (j = 0; j < 16; j++) {
                        for (i = 0; i < 16; i++) putchar(" .#"[s_ras[j * 16 + i]]);
                        putchar('\n');
                    }
                }
            }
        }
        if (e < TAGPU_ICON_EDGE_MIN) {
            int same = 0;
            for (c = 0; c < TAGPU_ICLS_N; c++)
                for (k = c + 1; k < TAGPU_ICLS_N; k++) same |= !memcmp(ras[c], ras[k], (size_t)e * e);
            CHECK(same, "every class is different at %d px: the floor is not the smallest such edge", e);
            continue;
        }
        for (c = 0; c < TAGPU_ICLS_N; c++)
            for (k = c + 1; k < TAGPU_ICLS_N; k++)
                CHECK(memcmp(ras[c], ras[k], (size_t)e * e) != 0, "%s and %s are the same at %d px",
                      NAME[c], NAME[k], e);
    }
    if (dump) printf("16-px FNV 0x%08X\n", h);
    CHECK(h == GOLDEN16, "the 16-px rasters' FNV is 0x%08X, golden 0x%08X", h, GOLDEN16);
}

int main(int argc, char** argv)
{
    const int dump = argc > 1 && !strcmp(argv[1], "dump");
    test_table();
    test_ramp();
    test_one();
    test_floor();
    test_shapes(dump);
    printf("icontest: %d check(s), %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
