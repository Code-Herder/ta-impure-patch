/* tagpu_gui_hook.c — the observers and the census (Phase E, G15a).
   Contract: inc/tagpu_gui.h. Design: research/notes/gui-renderer.md 3.5, 3.6.

   NOTHING HERE CHANGES WHAT THE ENGINE DRAWS. Every detour is an observer
   (tagpu_detour_observe): the original runs unchanged, we read its arguments
   off the stack on the way in and, where we need its result, on the way out.

   THE CENSUS. The engine's UI is retained: each .GUI screen owns a surface at
   panel+0xBC that the gadget handlers draw into, and the per-frame draw blits
   it into the frame (0x4AB0B0) only when dirty. So "which functions write UI
   pixels" cannot be answered by reading DrawGameScreen; it is answered by
   diffing. At every FlipOffscreenToPrimary (0x4C63A0, the engine's own
   "this frame is complete") the flipped surface is compared with its previous
   copy, the box of every op recorded since the last flip is subtracted, the
   world viewport is subtracted on a game frame (the terrain key fill is ours),
   and what is left is a writer we have not named — counted, boxed, and on
   demand written out as a PGM. The same diff runs on every surface an op has
   named (the GUI screens' own surfaces), so a handler that draws into a panel
   through a path we do not observe shows up there.

   THREADS. Everything in this file runs on the GAME thread, inside the
   engine's own calls; the render thread only reads a few counters in
   tagpu_gui_flush(). The op ring and the surface table are therefore plain
   statics with no locking, and an observer that fires on any other thread
   (tagpu_text.c rasterises through the glyph blitter on the render thread)
   records nothing: the game thread is the one the flip runs on. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_gui.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"

#define TA_MAINPP     0x00511DE8u
#define OFF_GUI_TOP   0x531           /* GUIInfo.TheActive_GUIMEM               */
#define GM_CTRLS      0x04
#define P_SURFACE     0xBC            /* panel record: OFFSCREEN* surface       */
#define P_TOTAL       0xB6

/* OFFSCREEN / drawing context head, shared by the surface object and the
   12-dword context the draw passes hand around (terrain-depth.md appendix) */
#define CTX_W      0
#define CTX_H      1
#define CTX_PITCH  2
#define CTX_BASE   3
#define CTX_CLIP_L 7                  /* +0x1C..+0x28, inclusive              */
#define CTX_CLIP_T 8
#define CTX_CLIP_R 9
#define CTX_CLIP_B 10

#define KEY_DEFAULT    254            /* tagpu_terr.c's composite key: the fill the
                                         terrain skip leaves in the viewport      */
#define FLIP_VA        0x004C63A0u    /* FlipOffscreenToPrimary                */
#define FLIP_RET_GAME  0x0046A3E0u    /* DrawGameScreen's call site returns here */
static const unsigned char FLIP_STOLEN[6] = { 0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------------ state */

static int      s_installed = 0;
static int      s_census = 0, s_log = 0, s_pgm = 0, s_trace = 0;
static int      s_key = KEY_DEFAULT;
static DWORD    s_gameTid = 0;        /* the thread the flip runs on          */
static volatile unsigned s_flips = 0, s_opsTotal = 0, s_opsDropped = 0;
static volatile unsigned s_unexplTotal = 0, s_changedTotal = 0;
static unsigned s_lastLog = 0;
static unsigned s_winChanged = 0, s_winUnexpl = 0, s_winCensus = 0;   /* since the last line */
static int      s_winL = 0x7FFF, s_winT = 0x7FFF, s_winR = -1, s_winB = -1;

static void glog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

/* ---- the surfaces we have seen (game thread only) ---------------------- */
typedef struct SURF {
    unsigned base;                    /* pixel base — the identity           */
    int w, h, pitch;
    unsigned char* copy;              /* the surface as of the last flip     */
    unsigned char* mask;              /* the last census: 0/128/255          */
    unsigned char* acc;               /* unexplained since the last PGM, OR-ed */
    int copyValid;
    unsigned seen;                    /* flips since first seen              */
    unsigned changed, explained, unexplained;   /* running totals            */
    int bl, bt, br, bb;               /* worst unexplained box last flip     */
} SURF;
#define MAX_SURF 24
static SURF s_surf[MAX_SURF];
static int  s_nsurf = 0;

static SURF* surf_get(unsigned base, int w, int h, int pitch)
{
    int i;
    if (!base || w <= 0 || h <= 0 || pitch <= 0 || w > 4096 || h > 4096 || pitch > 8192) return NULL;
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].base == base) {
            if (s_surf[i].w != w || s_surf[i].h != h || s_surf[i].pitch != pitch) {
                /* the object was re-allocated over the same bytes: start over */
                s_surf[i].w = w; s_surf[i].h = h; s_surf[i].pitch = pitch;
                s_surf[i].copyValid = 0;
                free(s_surf[i].copy); free(s_surf[i].mask); free(s_surf[i].acc);
                s_surf[i].copy = s_surf[i].mask = s_surf[i].acc = NULL;
            }
            return &s_surf[i];
        }
    if (s_nsurf >= MAX_SURF) return NULL;
    memset(&s_surf[s_nsurf], 0, sizeof(SURF));
    s_surf[s_nsurf].base = base; s_surf[s_nsurf].w = w; s_surf[s_nsurf].h = h;
    s_surf[s_nsurf].pitch = pitch;
    s_surf[s_nsurf].bl = s_surf[s_nsurf].bt = 0x7FFF; s_surf[s_nsurf].br = s_surf[s_nsurf].bb = -1;
    return &s_surf[s_nsurf++];
}

/* a drawing context's destination as a surface */
static SURF* surf_of_ctx(const int* ctx)
{
    if (!ptr_ok(ctx)) return NULL;
    return surf_get((unsigned)ctx[CTX_BASE], ctx[CTX_W], ctx[CTX_H], ctx[CTX_PITCH]);
}

/* ---- the ops recorded since the last flip ------------------------------ */
enum { OP_GAF = 1, OP_GAFA, OP_GAFB, OP_GAFD, OP_SCALE, OP_TEXT, OP_LINE, OP_BAR, OP_RECT, OP_FRAME, OP_FILL, OP_COPY, OP_NKIND };
static const char* const OP_NAME[OP_NKIND] = { "?", "gaf", "gafa", "gafb", "gafd", "scale", "text", "line", "bar", "rect", "frame", "fill", "copy" };
typedef struct OP { unsigned base; short l, t, r, b; unsigned char kind; } OP;
#define MAX_OPS 65536
static OP       s_ops[MAX_OPS];
static int      s_nops = 0;
static unsigned s_kindCount[OP_NKIND];
static unsigned s_nullCtx[OP_NKIND];        /* ops whose ctx was NULL/unknown */

static void op_add(int kind, SURF* s, int l, int t, int r, int b)
{
    OP* o;
    s_kindCount[kind]++;
    s_opsTotal++;
    if (!s) { s_nullCtx[kind]++; return; }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > s->w - 1) r = s->w - 1;
    if (b > s->h - 1) b = s->h - 1;
    if (l > r || t > b) return;
    if (s_nops >= MAX_OPS) { s_opsDropped++; return; }
    o = &s_ops[s_nops++];
    o->base = s->base; o->l = (short)l; o->t = (short)t; o->r = (short)r; o->b = (short)b;
    o->kind = (unsigned char)kind;
}

/* clip a box to the context's clip rect (inclusive) */
static void clip_ctx(const int* ctx, int* l, int* t, int* r, int* b)
{
    if (*l < ctx[CTX_CLIP_L]) *l = ctx[CTX_CLIP_L];
    if (*t < ctx[CTX_CLIP_T]) *t = ctx[CTX_CLIP_T];
    if (*r > ctx[CTX_CLIP_R]) *r = ctx[CTX_CLIP_R];
    if (*b > ctx[CTX_CLIP_B]) *b = ctx[CTX_CLIP_B];
}

static int on_game_thread(void)
{
    return s_gameTid != 0 && GetCurrentThreadId() == s_gameTid;
}

/* ---- the census, at the flip ------------------------------------------- */

static const char* top_screen_name(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const char* top;
    const char* ctrls;
    if (!ptr_ok(ta)) return "-";
    top = *(const char* const*)(ta + OFF_GUI_TOP);
    if (!ptr_ok(top)) return "(no gui)";
    ctrls = *(const char* const*)(top + GM_CTRLS);   /* ControlsAry[0] = the panel */
    if (!ptr_ok(ctrls)) return "(no panel)";
    return ctrls + 0x02;                            /* its name[16] is the screen's */
}

static void census_surface(SURF* s, int isGame, int subtractVp, int vl, int vt, int vr, int vb,
                           unsigned* outChanged, unsigned* outUnexpl)
{
    const unsigned char* cur = (const unsigned char*)(size_t)s->base;
    unsigned changed = 0, unexpl = 0;
    int y, x, i;
    *outChanged = *outUnexpl = 0;
    if (!ptr_ok(cur)) return;
    if (!s->copy) s->copy = (unsigned char*)malloc((size_t)s->w * s->h);
    if (!s->mask) s->mask = (unsigned char*)malloc((size_t)s->w * s->h);
    if (!s->copy || !s->mask) return;
    if (!s->copyValid) {
        for (y = 0; y < s->h; y++) memcpy(s->copy + (size_t)y * s->w, cur + (size_t)y * s->pitch, (size_t)s->w);
        memset(s->mask, 0, (size_t)s->w * s->h);
        s->copyValid = 1;
        return;
    }
    /* 1. what changed since the last flip */
    for (y = 0; y < s->h; y++) {
        const unsigned char* a = cur + (size_t)y * s->pitch;
        unsigned char* c = s->copy + (size_t)y * s->w;
        unsigned char* m = s->mask + (size_t)y * s->w;
        if (memcmp(a, c, (size_t)s->w) == 0) { memset(m, 0, (size_t)s->w); continue; }
        for (x = 0; x < s->w; x++) {
            if (a[x] != c[x]) { m[x] = 255; changed++; c[x] = a[x]; }
            else m[x] = 0;
        }
    }
    if (!changed) return;
    /* 2. subtract every op that named this surface */
    for (i = 0; i < s_nops; i++) {
        const OP* o = &s_ops[i];
        if (o->base != s->base) continue;
        for (y = o->t; y <= o->b; y++) {
            unsigned char* m = s->mask + (size_t)y * s->w;
            for (x = o->l; x <= o->r; x++) if (m[x] == 255) m[x] = 128;
        }
    }
    /* 3. on a game frame the world viewport is ours: the terrain skip fills it
       with the KEY every frame, so a changed pixel that is now the key is that
       erase. A changed pixel that is NOT the key inside the viewport is UI the
       engine drew over the world (a dialog, chat) and stays in the census. */
    if (isGame && subtractVp) {
        for (y = vt; y <= vb && y < s->h; y++) {
            unsigned char* m = s->mask + (size_t)y * s->w;
            const unsigned char* a = cur + (size_t)y * s->pitch;
            for (x = vl; x <= vr && x < s->w; x++) if (m[x] == 255 && a[x] == (unsigned char)s_key) m[x] = 128;
        }
    }
    /* 4. what is left */
    s->bl = s->bt = 0x7FFF; s->br = s->bb = -1;
    for (y = 0; y < s->h; y++) {
        const unsigned char* m = s->mask + (size_t)y * s->w;
        for (x = 0; x < s->w; x++) if (m[x] == 255) {
            unexpl++;
            if (x < s->bl) s->bl = x;
            if (x > s->br) s->br = x;
            if (y < s->bt) s->bt = y;
            if (y > s->bb) s->bb = y;
        }
    }
    s->changed += changed; s->unexplained += unexpl; s->explained += changed - unexpl;
    if (unexpl) {
        if (!s->acc) { s->acc = (unsigned char*)malloc((size_t)s->w * s->h); if (s->acc) memset(s->acc, 0, (size_t)s->w * s->h); }
        if (s->acc) for (i = 0; i < s->w * s->h; i++) if (s->mask[i] == 255) s->acc[i] = 255;
    }
    *outChanged = changed; *outUnexpl = unexpl;
}

/* the last census's mask (0/128) with every unexplained pixel since the last
   dump at 255; dumping clears the accumulation */
static void write_pgm(SURF* s)
{
    FILE* f;
    int y, x;
    if (!s || !s->mask) return;
    f = fopen("tagpu_gui_census.pgm", "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        unsigned char row[4096];
        const unsigned char* m = s->mask + (size_t)y * s->w;
        const unsigned char* a = s->acc ? s->acc + (size_t)y * s->w : NULL;
        for (x = 0; x < s->w; x++) row[x] = (a && a[x]) ? 255 : (m[x] == 255 ? 255 : m[x]);
        fwrite(row, 1, (size_t)s->w, f);
    }
    fclose(f);
    if (s->acc) memset(s->acc, 0, (size_t)s->w * s->h);
}

/* the flipped surface (tagpu_gui_leaves.h: the graphics globals' back buffer) */
static const int* flip_source(void* entry_esp);
static unsigned s_builds, s_buildFlags;     /* GUI_StageUpdateDraw calls since the last census */

#define CENSUS_MS 5      /* the shell flips ~5000x a second (measured 2026-09-07):
                            the diff runs at most this often, ops accumulate between */
#define LOG_EVERY 50     /* `log`: one line per this many censuses, or any unexplained */

static int __cdecl before_flip(void* entry_esp)
{
    unsigned ret = ((unsigned*)entry_esp)[0];
    const int* src;
    SURF* s;
    int isGame = (ret == FLIP_RET_GAME);
    unsigned changed = 0, unexpl = 0;
    char b[320];
    static LARGE_INTEGER s_lastQpc, s_freq;
    static unsigned s_censuses = 0;
    LARGE_INTEGER now;
    if (!s_gameTid) s_gameTid = GetCurrentThreadId();
    else if (!on_game_thread()) return 0;
    s_flips++;
    if (s_flips == 1) {
        _snprintf(b, sizeof b, "gui: first flip on thread %u (init saw %u)", (unsigned)GetCurrentThreadId(), (unsigned)s_gameTid);
        glog(b);
    }
    if (!s_census) { s_nops = 0; return 0; }
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&now);
    if (s_lastQpc.QuadPart && (now.QuadPart - s_lastQpc.QuadPart) * 1000 < (LONGLONG)CENSUS_MS * s_freq.QuadPart)
        return 0;                                   /* too soon: keep accumulating ops */
    s_lastQpc = now;
    s_censuses++;
    src = flip_source(entry_esp);
    s = surf_of_ctx(src);
    if (s) {
        int vl = 0, vt = 0, vr = -1, vb = -1, sub = 0;
        if (isGame) {
            const char* ta = *(const char* const*)TA_MAINPP;
            int L, T, W, H;
            tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
            if (W > 0 && H > 0) { vl = L; vt = T; vr = L + W - 1; vb = T + H - 1; sub = 1; }
        }
        s->seen++;
        census_surface(s, isGame, sub, vl, vt, vr, vb, &changed, &unexpl);
        s_changedTotal += changed; s_unexplTotal += unexpl;
        s_winChanged += changed; s_winUnexpl += unexpl; s_winCensus++;
        if (unexpl) {
            if (s->bl < s_winL) s_winL = s->bl;
            if (s->bt < s_winT) s_winT = s->bt;
            if (s->br > s_winR) s_winR = s->br;
            if (s->bb > s_winB) s_winB = s->bb;
        }
        if (s_pgm && GetFileAttributesA("tagpu_gui_census.trigger") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("tagpu_gui_census.trigger");
            write_pgm(s);
            _snprintf(b, sizeof b, "gui census: pgm written for surface %08X %dx%d (screen %s)",
                      s->base, s->w, s->h, top_screen_name());
            glog(b);
        }
    }
    /* every other surface an op named this frame: the GUI screens' own */
    {
        int i;
        for (i = 0; i < s_nsurf; i++) {
            unsigned c2, u2;
            if (s && s_surf[i].base == s->base) continue;
            census_surface(&s_surf[i], 0, 0, 0, 0, 0, 0, &c2, &u2);
            if (u2 && s_log && s_surf[i].h > 1) {
                int k, onThis = 0, shown = 0;
                for (k = 0; k < s_nops; k++) if (s_ops[k].base == s_surf[i].base) onThis++;
                _snprintf(b, sizeof b, "gui census: %s surface %08X %dx%d changed=%u unexplained=%u box=(%d,%d)-(%d,%d) ops_on_it=%d of %d",
                          top_screen_name(), s_surf[i].base, s_surf[i].w, s_surf[i].h, c2, u2,
                          s_surf[i].bl, s_surf[i].bt, s_surf[i].br, s_surf[i].bb, onThis, s_nops);
                glog(b);
                if (s_trace) {
                    /* the distinct destinations this window's ops named */
                    unsigned bases[16]; unsigned counts[16]; int nb = 0;
                    for (k = 0; k < s_nops; k++) {
                        int j;
                        for (j = 0; j < nb; j++) if (bases[j] == s_ops[k].base) { counts[j]++; break; }
                        if (j == nb && nb < 16) { bases[nb] = s_ops[k].base; counts[nb] = 1; nb++; }
                    }
                    for (k = 0; k < nb && shown < 16; k++, shown++) {
                        _snprintf(b, sizeof b, "gui trace: window ops on base %08X: %u", bases[k], counts[k]);
                        glog(b);
                    }
                }
            }
        }
    }
    if (s && s_trace && unexpl > 256) {
        /* the ops that touched the residual's box, up to 96 — the rest of the
           frame's ops are noise for placing it */
        int i, n = 0;
        for (i = 0; i < s_nops && n < 96; i++) {
            const OP* o = &s_ops[i];
            if (o->base != s->base) continue;
            if (o->r < s->bl || o->l > s->br || o->b < s->bt || o->t > s->bb) continue;
            _snprintf(b, sizeof b, "gui trace: %s %s op %s (%d,%d)-(%d,%d)", isGame ? "GAME" : "shell",
                      top_screen_name(), OP_NAME[o->kind], o->l, o->t, o->r, o->b);
            glog(b); n++;
        }
        for (i = 0; i < s_nsurf; i++) if (&s_surf[i] != s) {
            _snprintf(b, sizeof b, "gui trace: surface %08X %dx%d pitch %d", s_surf[i].base, s_surf[i].w, s_surf[i].h, s_surf[i].pitch);
            glog(b);
        }
    }
    if (s && (unexpl > 256 || s_censuses - s_lastLog >= (unsigned)(s_log ? LOG_EVERY : LOG_EVERY * 20))) {
        s_lastLog = s_censuses;
        {
            char ops[200]; int k, n = 0; unsigned nullc = 0;
            for (k = 1; k < OP_NKIND; k++) {
                nullc += s_nullCtx[k];
                if (s_kindCount[k]) n += _snprintf(ops + n, sizeof ops - (size_t)n, "%s%s %u", n ? " " : "", OP_NAME[k], s_kindCount[k]);
            }
            /* the numbers are the WINDOW's — every census since the previous line */
            _snprintf(b, sizeof b,
                "gui census: %s %s %08X %dx%d flip=%u n=%u win=%u changed=%u unexplained=%u box=(%d,%d)-(%d,%d) ops=%d[%s] "
                "builds=%u/%X nosurf=%u dropped=%u",
                isGame ? "GAME" : "shell", top_screen_name(), s->base, s->w, s->h, s_flips, s_censuses, s_winCensus,
                s_winChanged, s_winUnexpl,
                s_winUnexpl ? s_winL : 0, s_winUnexpl ? s_winT : 0, s_winUnexpl ? s_winR : 0, s_winUnexpl ? s_winB : 0,
                s_nops, ops, s_builds, s_buildFlags, nullc, s_opsDropped);
            glog(b);
            s_winChanged = s_winUnexpl = s_winCensus = 0;
            s_winL = s_winT = 0x7FFF; s_winR = s_winB = -1;
        }
    }
    s_nops = 0;
    memset(s_kindCount, 0, sizeof s_kindCount);
    memset(s_nullCtx, 0, sizeof s_nullCtx);
    s_builds = 0; s_buildFlags = 0;
    return 0;
}

/* ---- the leaf observers -------------------------------------------------
   Each reads the engine's arguments off entry_esp: [0] return address, [1]
   first stack argument, ... Conventions and boxes per leaf are in
   gui-renderer.md's appendix and the engine map; the byte strings are the
   prologues stolen, byte-matched at install. */

#include "tagpu_gui_leaves.h"

/* ---- install ----------------------------------------------------------- */

static int read_tokens(void)
{
    char buf[256];
    DWORD n = 0;
    HANDLE h = CreateFileA("tagpu_gui.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    buf[n] = 0;
    s_census = strstr(buf, "census") != NULL;
    s_log    = strstr(buf, "log") != NULL;
    s_pgm    = strstr(buf, "pgm") != NULL;
    s_trace  = strstr(buf, "trace") != NULL;
    { const char* k = strstr(buf, "key="); if (k) s_key = atoi(k + 4) & 255; }
    return 1;
}

void tagpu_gui_init(void)
{
    char b[200];
    int n, ok;
    if (!read_tokens()) return;
    /* all-or-nothing: every site must carry the bytes we expect */
    if (!tagpu_detour_bytes_ok(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN) || !leaves_match()) {
        glog("gui: NOT armed — engine bytes differ at a watched site");
        return;
    }
    /* DllMain runs on the process's first thread, which is the thread TA's
       game loop and every flip run on; taking it here rather than at the
       first flip means the splash screen's draws (before flip 1) are recorded */
    s_gameTid = GetCurrentThreadId();
    ok = tagpu_detour_observe(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN, before_flip, NULL);
    n = leaves_install();
    s_installed = ok && n == LEAF_COUNT;
    _snprintf(b, sizeof b, "gui: %s flip@0x4C63A0=%d leaves=%d/%d census=%d log=%d pgm=%d key=%d (Phase E G15a: observers only, nothing drawn)",
              s_installed ? "ARMED" : "FAILED", ok, n, LEAF_COUNT, s_census, s_log, s_pgm, s_key);
    glog(b);
}

int tagpu_gui_installed(void) { return s_installed; }

void tagpu_gui_flush(unsigned int frame_counter)
{
    static unsigned last = 0;
    char b[200];
    if (!s_installed) return;
    if (frame_counter - last >= 600) {
        last = frame_counter;
        _snprintf(b, sizeof b, "GUI flips=%u ops=%u dropped=%u changed=%u unexplained=%u surfaces=%d",
                  s_flips, s_opsTotal, s_opsDropped, s_changedTotal, s_unexplTotal, s_nsurf);
        glog(b);
    }
}
