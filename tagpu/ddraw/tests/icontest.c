/* icontest.c -- the strategic icons' rule and shapes (inc/tagpu_icon_rule.h),
   run on the host by tests/icontest.py, or under Wine as `make icontest`.

     1. the defaults' sizes and levels at 1920x1080, 2560x1440 and 3840x2160
        against the plan's table (research/notes/mega-map-plan.md, landing 3),
        to +-0.001;
     2. the fade ramp's two ends, and a zero band as a step;
     3. at 1x with the defaults, no icon -- alpha 0 for every class and the
        radar dot -- on every common display shape up to 4K native: the
        promise that 1x stays the stock frame;
     4. each shape rasterised at edges 4..128: symmetric about x = 0, ink on
        its border, from 16 px the plus at the centre of the builder classes
        alone and fill at the centre of the other solid ones, the nine
        silhouettes pairwise different at 8 px, and the 16-px rasters' FNV
        against a golden value.

   TWO PAIRS ARE NOT DIFFERENT AT 8 PX, and that is the shape rule's own
   geometry, not this test's: a hollow shape keeps its ink and a fill band 3w
   wide, w px each side of 4w in all, and an 8-px circle's radius is 3.7 px,
   so UNKNOWN is COMBAT and AIR is AIRCOMBAT there. On the triangle the band
   is wider than the shape's inradius (0.28 of the edge against 4w, about a
   third of it) at every size, so AIR's centre is filled at every edge; the
   circle's centre opens from 10 px wherever w rounds down. The test names
   both pairs and asserts the other 34.

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
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - v.band + 1e-4f), 10, 0, &hide);
        CHECK(a == 64, "fade %d: alpha %d at the band's start, want 64", FADE[i], a);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - v.band - 1e-4f), 10, 0, &hide);
        CHECK(a == 0, "fade %d: alpha %d before the band, want 0", FADE[i], a);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - 1e-4f), 10, 0, &hide);
        CHECK(a == 255 && !hide, "fade %d: alpha %d hide %d at the band's end, want 255, 0", FADE[i], a, hide);
        a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f + 1e-3f), 10, 0, &hide);
        CHECK(a == 255 && hide, "fade %d: alpha %d hide %d past the band, want 255, 1", FADE[i], a, hide);
        /* a model wider than 0.45 of the icon is never hidden */
        a = tagpu_icon_eval(&v, 0.05f, 10, 1000, &hide);
        CHECK(a == 255 && !hide, "fade %d: a large model hidden at 0.05", FADE[i]);
    }
    tagpu_icon_view(&v, 1920, 1080, 100, 100, 0);
    a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f - 1e-4f), 10, 0, &hide);
    CHECK(a == 0, "fade 0: alpha %d just before the switch, want 0", a);
    a = tagpu_icon_eval(&v, level_at(&v, 10, 1.0f + 1e-4f), 10, 0, &hide);
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
                const int al = tagpu_icon_eval(&v, 1.0f, e, 0, &hide);
                CHECK(al == 0 && !hide, "%dx%d at 1x: %s alpha %d", gw, gh, c < 0 ? "radar dot" : NAME[c], al);
            }
        }
}

/* ---- 4. the shapes ---- */
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

#define GOLDEN16 0x90B7B3E9u

static void test_shapes(int dump)
{
    static unsigned char eight[TAGPU_ICLS_N][64];
    int c, e, i, j, k;
    unsigned h = 2166136261u;
    for (c = 0; c < TAGPU_ICLS_N; c++)
        for (e = 4; e <= 128; e++) {
            int asym = 0, edgeInk = 1, any = 0;
            raster(c, e);
            for (j = 0; j < e; j++)
                for (i = 0; i < e; i++) {
                    if (s_ras[j * e + i] != s_ras[j * e + (e - 1 - i)]) asym++;
                    if (s_ras[j * e + i]) any = 1;
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
            CHECK(any, "%s at %d px: nothing lit", NAME[c], e);
            CHECK(!asym, "%s at %d px: %d texels differ from their mirror", NAME[c], e, asym);
            CHECK(edgeInk, "%s at %d px: a lit texel on the border is not ink", NAME[c], e);
            if (e >= 16) {
                /* the centre texels: the plus on a builder, fill on a solid
                   shape, nothing on a hollow one */
                const int m = s_ras[(e / 2) * e + e / 2];
                const int want = tagpu_icon_cls_plus(c) ? 2 : 1;
                if (!tagpu_icon_cls_hollow(c))
                    CHECK(m == want, "%s at %d px: centre %d, want %d", NAME[c], e, m, want);
            }
            if (e == 8) memcpy(eight[c], s_ras, 64);
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
    for (c = 0; c < TAGPU_ICLS_N; c++)
        for (k = c + 1; k < TAGPU_ICLS_N; k++) {
            const int twins = (c == TAGPU_ICLS_UNKNOWN && k == TAGPU_ICLS_COMBAT) ||
                              (c == TAGPU_ICLS_AIRCOMBAT && k == TAGPU_ICLS_AIR);
            if (twins) {
                if (!memcmp(eight[c], eight[k], 64))
                    printf("icontest: %s and %s are the same at 8 px (the hollow band fills it)\n",
                           NAME[c], NAME[k]);
                continue;
            }
            CHECK(memcmp(eight[c], eight[k], 64) != 0, "%s and %s are the same at 8 px", NAME[c], NAME[k]);
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
    test_shapes(dump);
    printf("icontest: %d check(s), %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
