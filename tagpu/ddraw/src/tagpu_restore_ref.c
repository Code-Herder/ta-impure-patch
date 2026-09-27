/* tagpu_restore_ref.c -- the restorer's launch self-test, CPU half
   (tagpu_restore_ref.h). No rendering API here, as there is none in the core.

   THE REFERENCE IS THE COMPUTE PATH'S DEFINITION, NOT ITS CODE. Every rule the
   shaders implement (tagpu_restore_comp.h) is restated here from the model and
   the padding rule, in plain loops: zero padding at every layer of a slot's
   rect, a wrap-padded rect for a frame that tiles and a centre crop after, the
   key stand-in FILL uses, OUT's rounding and its (0,0,0,0) at a key, the
   border OUT replicates and MIP's integer box. A device that disagrees with it
   by more than one level anywhere has computed something else. */

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "tagpu_restore_ref.h"

#define DIM  TAGPU_RPROBE_DIM
#define NF   TAGPU_RPROBE_NF

/* ---- the probe ------------------------------------------------------------ */

static unsigned lcg(unsigned* s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void frame(TAGPU_RGLSL_FRAME* f, int x, int y, int w, int h, int wrap, int key,
                  int padR, int padB)
{
    memset(f, 0, sizeof *f);
    f->ax = f->dx = x; f->ay = f->dy = y;
    f->w = w; f->h = h; f->wrap = wrap; f->key = key;
    f->border = 1; f->padR = padR; f->padB = padB;
}

/* A smooth field with a coarse ordered dither on top -- the kind of picture
   the network exists for -- so its outputs are not a constant and every
   channel of every layer carries something. Integers only: the same bytes on
   every compiler. */
void tagpu_rref_probe(TAGPU_RPROBE* p)
{
    static const unsigned char hue[16][3] = {
        {200, 60, 40}, {220, 140, 50}, {230, 210, 70}, {120, 190, 60},
        {50, 160, 80}, {40, 170, 170}, {50, 110, 200}, {90, 70, 190},
        {160, 60, 180}, {200, 70, 120}, {150, 110, 80}, {110, 140, 110},
        {140, 140, 150}, {90, 100, 120}, {190, 170, 140}, {70, 60, 50} };
    static const int bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    unsigned s = 0x7A5EEDu;
    int x, y, i, c;

    memset(p, 0, sizeof *p);
    for (i = 0; i < 256; i++) {
        int lv = (i & 15) + 1;
        for (c = 0; c < 3; c++) p->pal[i * 4 + c] = (unsigned char)(hue[i >> 4][c] * lv / 16);
    }
    for (y = 0; y < DIM; y++)
        for (x = 0; x < DIM; x++) {
            unsigned char* o = p->base + (y * DIM + x) * 4;
            int d = bayer[y & 3][x & 3] * 3 - 22;
            int n = (int)(lcg(&s) & 7) - 4;
            o[0] = clamp8(((x * 6 + y * 2 + d + n) / 8) * 8);
            o[1] = clamp8(((96 + y * 5 - x + d) / 8) * 8);
            o[2] = clamp8(((200 - x * 3 - y * 2 + d + n) / 8) * 8);
            o[3] = 255;
            p->r8[y * DIM + x] = (unsigned char)((((x + y * 3) >> 3) & 15) * 16 +
                                                 (((x * 2 + y + bayer[y & 3][x & 3]) >> 2) & 15));
        }

    /* the base job: a 30x26 keyed frame beside an 8x8 frame that wraps. Both
       are the 32 size class, so they share a batch of two slots and the grid
       is 96 rows: two conv bands. */
    frame(&p->fb[0], 1, 1, 30, 26, 0, 0, 1, 0);
    frame(&p->fb[1], 35, 1, 8, 8, 1, -1, 0, 0);
    /* the keys: a disc, a strip on the frame's bottom edge and lone texels */
    for (y = 0; y < 26; y++)
        for (x = 0; x < 30; x++) {
            int dx = x - 9, dy = y - 11;
            if (dx * dx + dy * dy <= 16 || (y >= 24 && x >= 18) ||
                (x == 25 && y == 4) || (x == 3 && y == 20))
                p->base[((1 + y) * DIM + 1 + x) * 4 + 3] = 0;
        }

    /* the palette job: a 20x14 frame keyed on index 7, beside a 6x8 frame
       that wraps -- again one batch of the 32 class */
    frame(&p->fr[0], 2, 2, 20, 14, 0, 7, 0, 1);
    frame(&p->fr[1], 30, 2, 6, 8, 1, -1, 0, 0);
    for (y = 0; y < 14; y++)
        for (x = 0; x < 20; x++)
            if ((x >= 12 && x < 17 && y >= 3 && y < 8) || (x == 2 && y == 10) || (x == 19 && y == 0))
                p->r8[(2 + y) * DIM + 2 + x] = 7;
}

int tagpu_rref_chain_bytes(void)
{
    int L, n = 0;
    for (L = 0; L <= TAGPU_RPROBE_MIPS; L++) n += (DIM >> L) * (DIM >> L) * 4;
    return n;
}

/* ---- the network ---------------------------------------------------------- */

typedef struct {
    int    depth;
    int    cin[TAGPU_R_MAXLAYERS], cout[TAGPU_R_MAXLAYERS];
    float* w[TAGPU_R_MAXLAYERS];      /* [tap][ci][co], tap = ky * 3 + kx          */
    float* b[TAGPU_R_MAXLAYERS];      /* [co]                                      */
} NET;

static void net_free(NET* n)
{
    int l;
    for (l = 0; l < TAGPU_R_MAXLAYERS; l++) { free(n->w[l]); free(n->b[l]); }
    memset(n, 0, sizeof *n);
}

/* THE WEIGHT FILE'S LAYOUT (unditherer/weights.py): layer l's output tile k is
   a block at offset + k x kstride vec4s, the bias in its first vec4, then a
   mat4 per (tap, input tile j) whose column i is input channel 4j + i across
   the four output channels 4k + 0..3. Every index is inside the body by the
   loader's bounds (tagpu_restore_core.c load_weights). */
static int net_copy(NET* n, const TAGPU_RMODEL* m)
{
    int l;
    memset(n, 0, sizeof *n);
    if (!m || !m->body || m->depth < 1 || m->depth > TAGPU_R_MAXLAYERS) return 0;
    n->depth = m->depth;
    for (l = 0; l < m->depth; l++) {
        const TAGPU_RLAYER* L = &m->layer[l];
        int cin = 4 * (int)L->jin, cout = 4 * (int)L->kout, tap, ci, co;
        n->cin[l] = cin; n->cout[l] = cout;
        n->w[l] = (float*)malloc((size_t)9 * cin * cout * sizeof(float));
        n->b[l] = (float*)malloc((size_t)cout * sizeof(float));
        if (!n->w[l] || !n->b[l]) { net_free(n); return 0; }
        for (co = 0; co < cout; co++) {
            size_t blk = (size_t)L->offset + (size_t)(co / 4) * L->kstride;
            n->b[l][co] = m->body[blk * 4 + (size_t)(co % 4)];
            for (tap = 0; tap < 9; tap++)
                for (ci = 0; ci < cin; ci++) {
                    size_t v = blk + 4u * (1u + (unsigned)tap * L->jin + (unsigned)(ci / 4)) + (unsigned)(ci % 4);
                    n->w[l][((size_t)tap * cin + ci) * cout + co] = m->body[v * 4 + (size_t)(co % 4)];
                }
        }
    }
    return 1;
}

/* One layer over an rw x rh rect, a tap outside the rect reading 0 -- the
   padding rule, at every layer. `in` is [rh][rw][cin], `out` [rh][rw][cout]. */
static void conv(const NET* n, int l, const float* in, float* out, int rw, int rh)
{
    const int cin = n->cin[l], cout = n->cout[l], last = l + 1 == n->depth;
    const float* W = n->w[l];
    float acc[256];
    int x, y, ky, kx, ci, co;
    for (y = 0; y < rh; y++)
        for (x = 0; x < rw; x++) {
            for (co = 0; co < cout; co++) acc[co] = n->b[l][co];
            for (ky = 0; ky < 3; ky++) {
                int sy = y + ky - 1;
                if (sy < 0 || sy >= rh) continue;
                for (kx = 0; kx < 3; kx++) {
                    int sx = x + kx - 1;
                    const float* a;
                    const float* wt;
                    if (sx < 0 || sx >= rw) continue;
                    a = in + ((size_t)sy * rw + sx) * cin;
                    wt = W + (size_t)(ky * 3 + kx) * cin * cout;
                    for (ci = 0; ci < cin; ci++) {
                        const float av = a[ci];
                        const float* wr = wt + (size_t)ci * cout;
                        if (av == 0.0f) continue;       /* ReLU leaves most of them */
                        for (co = 0; co < cout; co++) acc[co] += av * wr[co];
                    }
                }
            }
            for (co = 0; co < cout; co++)
                out[((size_t)y * rw + x) * cout + co] = last ? acc[co] : (acc[co] > 0.0f ? acc[co] : 0.0f);
        }
}

/* ---- one frame, as FILL -> the network -> OUT restore it ------------------ */

typedef struct {
    const unsigned char* atlas;       /* RGBA base, or R8 indices               */
    const unsigned char* pal;
    int base;
    const TAGPU_RGLSL_FRAME* f;
} SRC;

static int keyed(const SRC* s, int tx, int ty)
{
    int o = (s->f->ay + ty) * DIM + s->f->ax + tx;
    if (s->f->key < 0) return 0;
    return s->base ? s->atlas[o * 4 + 3] < 128 : s->atlas[o] == s->f->key;
}

static void colour(const SRC* s, int tx, int ty, float c[3])
{
    int o = (s->f->ay + ty) * DIM + s->f->ax + tx, i;
    const unsigned char* p = s->base ? s->atlas + o * 4 : s->pal + s->atlas[o] * 4;
    for (i = 0; i < 3; i++) c[i] = (float)p[i] / 255.0f;
}

static int fmod_i(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

static void tap(const SRC* s, int tx, int ty, int wrap, float acc[3], int* n)
{
    float c[3];
    const int w = s->f->w, h = s->f->h;
    if (wrap) { tx = fmod_i(tx, w); ty = fmod_i(ty, h); }
    else if (tx < 0 || ty < 0 || tx >= w || ty >= h) return;
    if (keyed(s, tx, ty)) return;
    colour(s, tx, ty, c);
    acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2];
    (*n)++;
}

/* FILL's stand-in for a keyed texel: the mean colour of the opaque texels on
   the nearest Chebyshev ring within R, walked in the shader's order */
static void stand_in(const SRC* s, int tx, int ty, int wrap, int R, float v[3])
{
    float acc[3] = { 0.0f, 0.0f, 0.0f };
    int n = 0, r, i;
    for (r = 1; r <= R; r++) {
        for (i = -r; i < r; i++) {
            tap(s, tx + i, ty - r, wrap, acc, &n);
            tap(s, tx + r, ty + i, wrap, acc, &n);
            tap(s, tx - i, ty + r, wrap, acc, &n);
            tap(s, tx - r, ty - i, wrap, acc, &n);
        }
        if (n > 0) break;
    }
    for (i = 0; i < 3; i++) v[i] = n > 0 ? acc[i] / (float)n : 0.0f;
}

/* The frame's restored interior, w x h RGBA, into `out`. 0 out of memory. */
static int restore_frame(const NET* n, const SRC* s, unsigned char* out)
{
    const TAGPU_RGLSL_FRAME* f = s->f;
    const int p = f->wrap ? n->depth : 0, rw = f->w + 2 * p, rh = f->h + 2 * p;
    int ch = 4, l, x, y, c;
    float* fill = (float*)malloc((size_t)f->w * f->h * 3 * sizeof(float));
    float* a;
    float* b;
    for (l = 0; l < n->depth; l++) if (n->cout[l] > ch) ch = n->cout[l];
    a = (float*)malloc((size_t)rw * rh * ch * sizeof(float));
    b = (float*)malloc((size_t)rw * rh * ch * sizeof(float));
    if (!fill || !a || !b) { free(fill); free(a); free(b); return 0; }

    for (y = 0; y < f->h; y++)
        for (x = 0; x < f->w; x++) {
            float* v = fill + ((size_t)y * f->w + x) * 3;
            if (keyed(s, x, y)) stand_in(s, x, y, p > 0, n->depth, v);
            else colour(s, x, y, v);
        }
    /* the input: RGB and a zero, the rect wrap-padded by floor division */
    for (y = 0; y < rh; y++)
        for (x = 0; x < rw; x++) {
            const float* v = fill + ((size_t)fmod_i(y - p, f->h) * f->w + fmod_i(x - p, f->w)) * 3;
            float* o = a + ((size_t)y * rw + x) * 4;
            o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = 0.0f;
        }
    for (l = 0; l < n->depth; l++) {
        float* t;
        conv(n, l, a, b, rw, rh);
        t = a; a = b; b = t;
    }
    /* OUT: the colour minus the network's output, rounded as the shader
       rounds; the key is (0, 0, 0, 0). `a` is now [rh][rw][4]. */
    for (y = 0; y < f->h; y++)
        for (x = 0; x < f->w; x++) {
            unsigned char* o = out + ((size_t)y * f->w + x) * 4;
            const float* nv = a + ((size_t)(y + p) * rw + x + p) * n->cout[n->depth - 1];
            float col[3];
            if (keyed(s, x, y)) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            colour(s, x, y, col);
            for (c = 0; c < 3; c++) {
                float k = (float)floor((double)((col[c] - nv[c]) * 255.0f + 0.5f));
                o[c] = (unsigned char)(k < 0.0f ? 0 : k > 255.0f ? 255 : (int)k);
            }
            o[3] = 255;
        }
    free(fill); free(a); free(b);
    return 1;
}

/* ---- the worker and the verdict ------------------------------------------- */

struct TAGPU_RTEST {
    HANDLE        th;
    NET           net;
    TAGPU_RPROBE  probe;
    unsigned char* exp[2 * NF];       /* fb[0..NF-1], then fr[0..NF-1]          */
    volatile LONG status;             /* 0 running, 1 done, -1 out of memory    */
    double        ms;
};

static DWORD WINAPI worker(LPVOID arg)
{
    TAGPU_RTEST* t = (TAGPU_RTEST*)arg;
    LARGE_INTEGER f0, c0, c1;
    int k, ok = 1;
    QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&c0);
    for (k = 0; k < 2 * NF && ok; k++) {
        SRC s;
        s.base = k < NF;
        s.atlas = s.base ? t->probe.base : t->probe.r8;
        s.pal = t->probe.pal;
        s.f = s.base ? &t->probe.fb[k] : &t->probe.fr[k - NF];
        t->exp[k] = (unsigned char*)malloc((size_t)s.f->w * s.f->h * 4);
        ok = t->exp[k] && restore_frame(&t->net, &s, t->exp[k]);
    }
    QueryPerformanceCounter(&c1);
    t->ms = 1000.0 * (double)(c1.QuadPart - c0.QuadPart) / (double)f0.QuadPart;
    InterlockedExchange(&t->status, ok ? 1 : -1);
    return 0;
}

TAGPU_RTEST* tagpu_rref_test_start(const TAGPU_RMODEL* m, const TAGPU_RPROBE* p)
{
    TAGPU_RTEST* t = (TAGPU_RTEST*)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->probe = *p;
    if (!net_copy(&t->net, m)) { free(t); return NULL; }
    t->th = CreateThread(NULL, 0, worker, t, 0, NULL);
    if (!t->th) { net_free(&t->net); free(t); return NULL; }
    return t;
}

int tagpu_rref_test_done(TAGPU_RTEST* t)
{
    return t && t->th && WaitForSingleObject(t->th, 0) == WAIT_OBJECT_0;
}

void tagpu_rref_test_free(TAGPU_RTEST* t)
{
    int k;
    if (!t) return;
    if (t->th) { WaitForSingleObject(t->th, INFINITE); CloseHandle(t->th); }
    for (k = 0; k < 2 * NF; k++) free(t->exp[k]);
    net_free(&t->net);
    free(t);
}

/* One destination against the expected interiors of its frames: every texel
   of every frame's cell -- the interior, the replicated border and the slack
   -- is its clamped interior texel's expected value, within one level on RGB
   and exactly on alpha. */
static int check_frames(const TAGPU_RGLSL_FRAME* fs, unsigned char* const* exp,
                        const unsigned char* got, long* bytes, int* worst, char* why, int whyLen)
{
    int k, qx, qy, c;
    for (k = 0; k < NF; k++) {
        const TAGPU_RGLSL_FRAME* f = &fs[k];
        const int qw = f->w + 2 * f->border + f->padR, qh = f->h + 2 * f->border + f->padB;
        for (qy = 0; qy < qh; qy++)
            for (qx = 0; qx < qw; qx++) {
                int gx = f->dx - f->border + qx, gy = f->dy - f->border + qy;
                int ix = gx - f->dx, iy = gy - f->dy;
                const unsigned char* e;
                const unsigned char* g;
                if (gx < 0 || gy < 0 || gx >= DIM || gy >= DIM) continue;
                ix = ix < 0 ? 0 : ix >= f->w ? f->w - 1 : ix;
                iy = iy < 0 ? 0 : iy >= f->h ? f->h - 1 : iy;
                e = exp[k] + ((size_t)iy * f->w + ix) * 4;
                g = got + ((size_t)gy * DIM + gx) * 4;
                for (c = 0; c < 4; c++) {
                    int d = abs((int)g[c] - (int)e[c]);
                    (*bytes)++;
                    if (d > *worst) *worst = d;
                    if (c == 3 ? d != 0 : d > 1) {
                        _snprintf(why, whyLen, "frame %d at (%d,%d) channel %d: %d, the reference %d",
                                  k, gx, gy, c, g[c], e[c]);
                        return 0;
                    }
                }
            }
    }
    return 1;
}

int tagpu_rref_test_check(TAGPU_RTEST* t, const unsigned char* gotB,
                          const unsigned char* gotR, char* why, int whyLen)
{
    long bytes = 0;
    int worst = 0, L, off = 0;
    char w[160];
    if (why && whyLen > 0) why[0] = 0;
    if (!t || t->status != 1) {
        _snprintf(why, whyLen, "the CPU reference could not be computed");
        return -1;
    }
    if (!check_frames(t->probe.fb, t->exp, gotB, &bytes, &worst, w, sizeof w)) {
        _snprintf(why, whyLen, "base job: %s", w);
        return 0;
    }
    if (!check_frames(t->probe.fr, t->exp + NF, gotR, &bytes, &worst, w, sizeof w)) {
        _snprintf(why, whyLen, "palette job: %s", w);
        return 0;
    }
    /* THE CHAIN, EXACTLY: each level the integer (sum + 1) / 4 of the 2 x 2
       box of the level above it, as read back -- MIP's own rule, so a driver
       that rounds its own way is caught here and nowhere else */
    for (L = 1; L <= TAGPU_RPROBE_MIPS; L++) {
        const int up = DIM >> (L - 1), dn = DIM >> L;
        const unsigned char* a = gotB + off;
        const unsigned char* b = gotB + off + up * up * 4;
        int x, y, c;
        for (y = 0; y < dn; y++)
            for (x = 0; x < dn; x++)
                for (c = 0; c < 4; c++) {
                    int s = a[((2 * y) * up + 2 * x) * 4 + c] + a[((2 * y) * up + 2 * x + 1) * 4 + c] +
                            a[((2 * y + 1) * up + 2 * x) * 4 + c] + a[((2 * y + 1) * up + 2 * x + 1) * 4 + c];
                    bytes++;
                    if (b[(y * dn + x) * 4 + c] != (s + 1) / 4) {
                        _snprintf(why, whyLen, "mip level %d at (%d,%d) channel %d: %d, the box %d",
                                  L, x, y, c, b[(y * dn + x) * 4 + c], (s + 1) / 4);
                        return 0;
                    }
                }
        off += up * up * 4;
    }
    _snprintf(why, whyLen, "%ld bytes within %d level(s) of the CPU reference, computed in %.0f ms",
              bytes, worst, t->ms);
    return 1;
}
