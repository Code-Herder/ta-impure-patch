#include <windows.h>
#include <string.h>
#include "tagpu_r3dcache.h"

#define NENT 512
#define GF_WIDTH     0x00
#define GF_HEIGHT    0x02
#define GF_COMPRESSED 0x09
#define GF_PTRCOLOR  0x10
#define GF_PTRDEPTH  0x14

#define NVAR 16
typedef struct {
    unsigned char* col;          /* w*h */
    unsigned char* dep;          /* w*h */
    int            cap;          /* allocated pixels */
    int            hx, hy;       /* hotspot at store time */
    unsigned       stamp;        /* recency */
    volatile int   w, h;         /* valid dims (0 = empty) */
} Var;
typedef struct {
    const void*    key;          /* Object3do* */
    Var            v[NVAR];      /* recent composite-box variants (a spinning
                                    piece cycles a small set of AABBs; exact-box
                                    restore beats overlap-paste) */
} Ent;
static unsigned s_clock = 0;

static Ent s_e[NENT];
static void* s_grave[64];        /* delayed-free ring for grown buffers */
static int   s_gi = 0;
static volatile unsigned s_restored = 0, s_missed = 0;
volatile unsigned g_rc_calls = 0, g_rc_off = 0, g_rc_badptr = 0;

static int ptr_ok(unsigned int p) { return p > 0x00600000u && p < 0x7FFF0000u; }

static Ent* slot(const void* key, int make)
{
    unsigned h = ((unsigned)(size_t)key >> 4) & (NENT - 1);
    unsigned i;
    for (i = 0; i < NENT; i++) {
        Ent* e = &s_e[(h + i) & (NENT - 1)];
        if (e->key == key) return e;
        if (!e->key) return make ? (e->key = key, e) : 0;
    }
    return 0;
}

void tagpu_r3dcache_store(const void* obj3do, const unsigned char* col,
                          const unsigned char* dep, int w, int h, int hx, int hy)
{
    if (!obj3do || w <= 0 || h <= 0 || w > 1280 || h > 1280) return;
    Ent* e = slot(obj3do, 1);
    if (!e) return;
    /* pick the variant: same box if we have it, else empty, else LRU */
    Var* v = 0;
    int i;
    for (i = 0; i < NVAR; i++)
        if (e->v[i].w == w && e->v[i].h == h &&
            e->v[i].hx == hx && e->v[i].hy == hy) { v = &e->v[i]; break; }
    if (!v) for (i = 0; i < NVAR; i++) if (!e->v[i].w && !e->v[i].col) { v = &e->v[i]; break; }
    if (!v) {
        v = &e->v[0];
        for (i = 1; i < NVAR; i++) if (e->v[i].stamp < v->stamp) v = &e->v[i];
    }
    int need = w * h;
    if (need > v->cap) {
        unsigned char* nc = (unsigned char*)malloc(need);
        unsigned char* nd = (unsigned char*)malloc(need);
        if (!nc || !nd) { free(nc); free(nd); return; }
        v->w = 0;                                  /* invalidate for readers */
        if (v->col) { free(s_grave[s_gi & 63]); s_grave[s_gi++ & 63] = v->col; }
        if (v->dep) { free(s_grave[s_gi & 63]); s_grave[s_gi++ & 63] = v->dep; }
        v->col = nc; v->dep = nd; v->cap = need;
    }
    v->w = 0;                                      /* readers skip mid-write */
    memcpy(v->col, col, need);
    memcpy(v->dep, dep, need);
    v->hx = hx; v->hy = hy;
    v->stamp = ++s_clock;
    v->h = h;
    v->w = w;                                      /* publish */
}

int tagpu_r3dcache_restore(unsigned int obj3do, unsigned int frame)
{
    /* runtime A/B: tagpu_r3dcache.off disables restores (re-checked every 256
       calls — this runs on the game thread, keep filesystem probes rare) */
    static int off = -1; static unsigned ctr = 0;
    g_rc_calls++;
    if (off < 0 || (++ctr & 0xFF) == 0)
        off = GetFileAttributesA("tagpu_r3dcache.off") != INVALID_FILE_ATTRIBUTES;
    if (off) { g_rc_off++; return 0; }
    if (!ptr_ok(obj3do) || !ptr_ok(frame)) { g_rc_badptr++; return 0; }
    Ent* e = slot((const void*)(size_t)obj3do, 0);
    if (!e) { s_missed++; return 0; }
    if (*(unsigned char*)(frame + GF_COMPRESSED) != 0) { s_missed++; return 0; }
    int w = *(unsigned short*)(frame + GF_WIDTH);
    int h = *(unsigned short*)(frame + GF_HEIGHT);
    int hx = *(short*)(frame + 0x04), hy = *(short*)(frame + 0x06);
    if (w <= 0 || h <= 0 || w > 1280 || h > 1280) { s_missed++; return 0; }
    unsigned char* c = *(unsigned char**)(frame + GF_PTRCOLOR);
    unsigned char* d = *(unsigned char**)(frame + GF_PTRDEPTH);
    if (!ptr_ok((unsigned int)(size_t)c) || IsBadWritePtr(c, (SIZE_T)w * h)) { s_missed++; return 0; }
    int dOk = ptr_ok((unsigned int)(size_t)d) && !IsBadWritePtr(d, (SIZE_T)w * h);
    /* exact-box variant first (spinning pieces cycle few discrete boxes) */
    Var* best = 0; int i;
    for (i = 0; i < NVAR; i++) {
        Var* v = &e->v[i];
        if (v->w == w && v->h == h && v->hx == hx && v->hy == hy &&
            v->w * v->h <= v->cap) { best = v; break; }
    }
    if (best) {
        memcpy(c, best->col, (size_t)w * h);
        if (dOk) memcpy(d, best->dep, (size_t)w * h);
        s_restored++;
        return 1;
    }
    /* else: hotspot-aligned overlap paste from the variant that COVERS the
       most of the new box (the widest cached pose usually covers ~all of it;
       "newest" can be a narrow pose and leaves visible holes for a frame) */
    {
        long bestCov = -1;
        for (i = 0; i < NVAR; i++) {
            Var* v = &e->v[i];
            if (v->w <= 0 || v->w * v->h > v->cap) continue;
            int oxc = hx - v->hx, oyc = hy - v->hy;
            int ix0 = oxc > 0 ? oxc : 0, iy0 = oyc > 0 ? oyc : 0;
            int ix1 = oxc + v->w < w ? oxc + v->w : w;
            int iy1 = oyc + v->h < h ? oyc + v->h : h;
            long cov = (ix1 > ix0 && iy1 > iy0) ? (long)(ix1 - ix0) * (iy1 - iy0) : 0;
            if (cov > bestCov) { bestCov = cov; best = v; }
        }
    }
    if (!best) { s_missed++; return 0; }
    int ew = best->w, eh = best->h, ehx = best->hx, ehy = best->hy;
    const unsigned char* ecol = best->col;
    const unsigned char* edep = best->dep;
    if (ew <= 0) { s_missed++; return 0; }
    /* box changed (e.g. a spinning arm alters the AABB): hotspot-aligned
       overlap copy so the unit body stays visible this frame */
    memset(c, 1, (size_t)w * h);                    /* ColorKey background */
    if (dOk) memset(d, 0, (size_t)w * h);
    int ox = hx - ehx, oy = hy - ehy;               /* old(0,0) inside new box */
    int x0 = ox < 0 ? -ox : 0, y0 = oy < 0 ? -oy : 0;             /* src start */
    int x1 = ew, y1 = eh;
    if (ox + x1 > w) x1 = w - ox;
    if (oy + y1 > h) y1 = h - oy;
    if (x1 > x0 && y1 > y0) {
        int y;
        for (y = y0; y < y1; y++) {
            memcpy(c + (size_t)(y + oy) * w + (x0 + ox), ecol + (size_t)y * ew + x0, (size_t)(x1 - x0));
            if (dOk)
                memcpy(d + (size_t)(y + oy) * w + (x0 + ox), edep + (size_t)y * ew + x0, (size_t)(x1 - x0));
        }
    }
    s_restored++;
    return 1;
}

void tagpu_r3dcache_stats(unsigned* restored, unsigned* missed)
{
    *restored = s_restored; *missed = s_missed;
    s_restored = 0; s_missed = 0;
}

/* Wipe a composite's planes to ColorKey/far — used when the NATIVE pass owns
   the unit: the engine keeps building/blitting the composite, and an empty
   plane makes that blit a no-op (colour-keyed). Game-thread safe (memset). */
void tagpu_r3dcache_wipe(unsigned int frame)
{
    if (!ptr_ok(frame)) return;
    if (*(unsigned char*)(frame + GF_COMPRESSED) != 0) return;
    int w = *(unsigned short*)(frame + GF_WIDTH);
    int h = *(unsigned short*)(frame + GF_HEIGHT);
    if (w <= 0 || h <= 0 || w > 1280 || h > 1280) return;
    unsigned char* c = *(unsigned char**)(frame + GF_PTRCOLOR);
    unsigned char* d = *(unsigned char**)(frame + GF_PTRDEPTH);
    if (ptr_ok((unsigned int)(size_t)c) && !IsBadWritePtr(c, (SIZE_T)w * h))
        memset(c, 1, (size_t)w * h);
    if (ptr_ok((unsigned int)(size_t)d) && !IsBadWritePtr(d, (SIZE_T)w * h))
        memset(d, 0, (size_t)w * h);
}
