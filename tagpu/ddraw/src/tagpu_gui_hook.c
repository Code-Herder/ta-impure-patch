/* tagpu_gui_hook.c — the observers, the census and the publisher (Phase E,
   G15a + G15b). Contract: inc/tagpu_gui.h. Design: research/notes/gui-renderer.md
   3.5, 3.6, 10.

   NOTHING HERE CHANGES WHAT THE ENGINE DRAWS. Every detour is an observer
   (tagpu_detour_observe): the original runs unchanged, we read its arguments
   off the stack on the way in and, where we need its result, on the way out.

   THE PUBLISHER (G15b). While the layer is on (g_gui_draw, the render thread's
   poll of tagpu_gui.on), the same ring the census reads is turned into queue
   ops for tagpu_gui_surf.c at the census cadence, inside the flip observer:
   a seed for every surface first seen, a sprite for a plain keyed GAF blit
   (its bytes decoded here on first sight), a twin-to-twin copy for 0x4C6B70
   from a twinned source, the viewport clear at the terrain key fill, and the
   box's bytes — read NOW, the frame complete — for everything else. See
   publish() and the dedup note above it.

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
#include "tagpu_opt.h"
#include "tagpu_gui_int.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"
#include "tagpu_gaf.h"
#include "tagpu_terrown.h"

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
/* `nostring` (G17d): text stays a box of captured pixels, as it was through
   the whole of phase 1. The A/B for the arena saving, and the escape if the
   stamp ever disagrees with the engine's blit on some font.
   READ AT ATTACH, LIKE EVERY OTHER TOKEN THIS FILE OWNS — `read_tokens` runs
   once, from `tagpu_gui_init`, so `census`, `log`, `pgm`, `trace` and this one
   must be armed BEFORE the launch. Only the surf module's tokens (`strict`,
   `norestore`, `sharptest`, `nocursor`, `cursorscale=`) follow the file live,
   because only the DRAW can change mid-session; the publisher's shape cannot
   without leaving the twins holding ops of the other kind. Arming it on a
   running instance silently does nothing, which cost one A/B to notice. */
static int      s_nostring = 0;
static int      s_key = KEY_DEFAULT;
static int      s_probeX = -1, s_probeY = -1;   /* trace: ops touching this pixel */
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
static void ops_forget_base(unsigned base);       /* below, with the ring */
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
    int seeded;                       /* a PK_SEED was published for it      */
    int lastCopyFrom;                 /* dedup(): position in the batch of the last
                                         COPY that read this surface, -1 if none  */
    int isOffscreen;                  /* created with the tag "OFFSCREEN" (0x5091D4): THE
                                         main offscreen, of which the engine has one at a
                                         time — see surf_drop_offscreens             */
} SURF;
#define MAX_SURF 24
static SURF s_surf[MAX_SURF];
static int  s_nsurf = 0;
#define TAG_OFFSCREEN 0x005091D4u     /* the "OFFSCREEN" string every 0x4C69F0 of the main
                                         offscreen pushes: 0x490AD3, 0x491250, 0x491B23,
                                         0x4980CF, 0x498402                              */

static TAGPU_PUBOP* pub_op(int kind, unsigned surf);   /* below */
static void pub_commit(void);
static void ops_forget_base(unsigned base);
extern volatile int g_gui_draw;

/* forget a surface: its buffers, its recorded boxes, and the twin */
static void surf_drop(int i)
{
    if (s_surf[i].seeded && g_gui_draw) {
        TAGPU_PUBOP* o = pub_op(PK_FREE, s_surf[i].base);
        if (o) pub_commit();
    }
    free(s_surf[i].copy); free(s_surf[i].mask); free(s_surf[i].acc);
    ops_forget_base(s_surf[i].base);
    s_surf[i] = s_surf[--s_nsurf];
}

/* THE MAIN OFFSCREEN IS FREED TO THE HEAP, NOT THROUGH SurfaceFree: MEM_Free
   0x4D85A0 at 0x491AB8 (leaving a game) and 0x49838C (the game's mode switch),
   and the next 0x4C69F0("OFFSCREEN", w, h) may land on the same base (a
   same-base size change, surf_get) or on another (MEASURED 2026-09-07, both).
   In the second case the old entry stayed: a dead 1024x768 the census walked
   at every flip — an access violation at its base once the heap had returned
   the block (the census run's crash on the first game -> shell switch) — and
   one MAX_SURF slot leaked per cycle. The engine has exactly one main
   offscreen at a time, so a new one retires every other. Keyed on the base,
   NOT a SURF* — surf_drop swap-removes (s_surf[i] = s_surf[--s_nsurf]), so a
   pointer to the kept entry moves if it was the last slot; the base is stable. */
static void surf_drop_offscreens(unsigned keepBase)
{
    int i;
    for (i = 0; i < s_nsurf; ) {
        if (s_surf[i].base != keepBase && s_surf[i].isOffscreen) surf_drop(i);
        else i++;
    }
}

static SURF* surf_get(unsigned base, int w, int h, int pitch)
{
    int i;
    if (!base || w <= 0 || h <= 0 || pitch <= 0 || w > 4096 || h > 4096 || pitch > 8192) return NULL;
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].base == base) {
            if (s_surf[i].w != w || s_surf[i].h != h || s_surf[i].pitch != pitch) {
                /* the object was re-allocated over the same bytes: start over.
                   The ops already recorded against the base carry the OLD
                   size's boxes, and nothing else drops them: the game's own
                   OFFSCREEN is freed by 0x4D85A0 directly (0x491AB8 on the way
                   back to the shell, 0x49838C at the game's mode switch), never
                   through SurfaceFree 0x4C6AC0, so before_free never sees it —
                   the next publish then found a box past the new surface and
                   called it an overflow (MEASURED 2026-09-07, one per switch) */
                ops_forget_base(base);
                s_surf[i].w = w; s_surf[i].h = h; s_surf[i].pitch = pitch;
                s_surf[i].copyValid = 0;
                s_surf[i].seeded = 0;          /* the twin is the old size: re-make it */
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
enum { OP_GAF = 1, OP_GAFA, OP_GAFB, OP_GAFD, OP_SCALE, OP_TEXT, OP_LINE, OP_BAR, OP_RECT, OP_FRAME, OP_FILL, OP_COPY,
       OP_FLIP, OP_NKIND };
static const char* const OP_NAME[OP_NKIND] = { "?", "gaf", "gafa", "gafb", "gafd", "scale", "text", "line", "bar", "rect", "frame", "fill", "copy", "flip" };
typedef struct OP {
    unsigned base; short l, t, r, b; unsigned char kind;
    /* what the publisher needs beyond the box (gui-renderer.md 3.6) */
    unsigned char ck; unsigned short fw, fh;    /* sprite: key, frame size    */
    const void* frame; const void* pix;         /* sprite: identity           */
    short dx, dy;                               /* sprite: unclipped top-left */
    unsigned src; short sl, st;                 /* copy: source, its top-left */
    unsigned seq;                               /* flip: terrown's fill seq   */
    /* text (G17d): the string is copied into a game-thread scratch AT OBSERVE
       TIME, not read again at publish. The argument routinely points at a
       caller's stack temp, which is gone by the flip — the same reason a
       sprite's pixels are copied rather than pointed at (gui-renderer.md 3.5).
       `frame` carries the font object and `dx`/`dy` the x/y it was given. */
    unsigned soff; unsigned short slen;
    unsigned char fg, bg, tr;                   /* text: 0x4CCF60's three colours */
    unsigned char dup;                          /* an identical op follows: dropped */
} OP;
/* The batch's strings. Reset with s_nops, and bounded the same way: a census is
   ~5 ms of drawing, in which the whole UI redraws a few hundred short labels. */
#define STR_SCRATCH (64u << 10)
static unsigned char s_strBuf[STR_SCRATCH];
static unsigned s_strUsed;
static unsigned s_strLost;                      /* strings the scratch could not take */
static OP* s_lastOp = NULL;                     /* the op op_add just recorded */
#define MAX_OPS 65536
static OP       s_ops[MAX_OPS];
static int      s_nops = 0;
/* ops recorded against a base are dead once the object is gone or re-made: a
   new surface may be allocated over the same bytes before the next census, and
   neither the census nor the publisher may apply an old box to it */
static void ops_forget_base(unsigned base)
{
    int k;
    for (k = 0; k < s_nops; k++) if (s_ops[k].base == base) s_ops[k].base = 0;
}
static unsigned s_kindCount[OP_NKIND];
static unsigned s_kindTotal[OP_NKIND];          /* cumulative, for the heartbeat */
static unsigned s_nullCtx[OP_NKIND];        /* ops whose ctx was NULL/unknown */

static void op_add(int kind, SURF* s, int l, int t, int r, int b)
{
    OP* o;
    /* EVERY early return below must leave s_lastOp NULL: the callers (gaf_box,
       before_copy) decorate "the op just recorded" with the frame identity or
       the copy's source, and a blit that recorded nothing — a fully clipped
       glyph, a NULL surface — must not write those into the PREVIOUS op.
       (MEASURED 2026-09-07: the last glyph of ARMOPT's "Exit" label lost its
       frame to the fully clipped blit that followed it — 67 px at 1024×768.) */
    s_lastOp = NULL;
    s_kindCount[kind]++;
    s_kindTotal[kind]++;
    s_opsTotal++;
    if (!s) { s_nullCtx[kind]++; return; }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > s->w - 1) r = s->w - 1;
    if (b > s->h - 1) b = s->h - 1;
    if (l > r || t > b) return;
    if (s_nops >= MAX_OPS) { s_opsDropped++; return; }
    o = &s_ops[s_nops++];
    memset(o, 0, sizeof *o);
    o->base = s->base; o->l = (short)l; o->t = (short)t; o->r = (short)r; o->b = (short)b;
    o->kind = (unsigned char)kind;
    s_lastOp = o;
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

/* ---- the publisher (game thread -> tagpu_gui_surf.c) ------------------- */

TAGPU_GUIQ g_guiq;                    /* the queue; storage below              */
static TAGPU_PUBOP    s_qops[TAGPU_GUI_QCAP];
static unsigned char* s_arena;
volatile int g_gui_draw = 0;          /* set by the render thread's trigger poll */
static int   s_pubOverflow = 0;
static unsigned s_pubOps = 0, s_pubBytes = 0;

/* sprite frames whose bytes were already published (open addressing) */
#define SEEN_N 8192
static const void* s_seenF[SEEN_N];
static const void* s_seenP[SEEN_N];
static unsigned hash_ptr(const void* a, const void* b)
{
    unsigned x = (unsigned)(size_t)a * 2654435761u ^ ((unsigned)(size_t)b >> 3) * 40503u;
    return (x ^ (x >> 15)) & (SEEN_N - 1);
}
static int seen_frame(const void* f, const void* p, int add)
{
    unsigned i = hash_ptr(f, p), n;
    for (n = 0; n < SEEN_N; n++, i = (i + 1) & (SEEN_N - 1)) {
        if (!s_seenF[i]) { if (add) { s_seenF[i] = f; s_seenP[i] = p; } return 0; }
        if (s_seenF[i] == f && s_seenP[i] == p) return 1;
    }
    return 1;                          /* full: claim seen, the consumer copes */
}

static TAGPU_PUBOP* pub_op(int kind, unsigned surf)
{
    unsigned head = g_guiq.qHead, tail = g_guiq.qTail;
    TAGPU_PUBOP* o;
    if (head - tail >= TAGPU_GUI_QCAP - 1) { s_pubOverflow = 1; g_guiq.why = TAGPU_GUI_WHY_QUEUE; return NULL; }
    o = &s_qops[head & (TAGPU_GUI_QCAP - 1)];
    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)kind; o->surf = surf; o->flip = s_flips;
    return o;
}
static void pub_commit(void) { MemoryBarrier(); g_guiq.qHead++; s_pubOps++; }

/* bytes for an op: the arena slot, or NULL when there is no room */
static unsigned char* pub_bytes(TAGPU_PUBOP* o, unsigned len)
{
    unsigned at;
    if (!tagpu_guiq_arena_room(&g_guiq, len, &at)) { s_pubOverflow = 1; g_guiq.why = TAGPU_GUI_WHY_ARENA; return NULL; }
    o->aoff = at; o->alen = len;
    g_guiq.aHead = at + len;
    s_pubBytes += len;
    return s_arena + at;
}

/* a surface's bytes, rows packed: the seed, or a box */
static int pub_surface_bytes(SURF* s, int l, int t, int r, int b, TAGPU_PUBOP* o)
{
    const unsigned char* cur = (const unsigned char*)(size_t)s->base;
    unsigned w = (unsigned)(r - l + 1), hh = (unsigned)(b - t + 1);
    unsigned char* dst;
    int y;
    if (!ptr_ok(cur) || l < 0 || t < 0 || r >= s->w || b >= s->h || l > r || t > b) {
        /* a box recorded against a surface that has since changed size (or a
           base we cannot read): the batch stops here and the next publish
           starts fresh — never a silent drop that leaves a twin stale */
        s_pubOverflow = 1; g_guiq.why = TAGPU_GUI_WHY_BOX;
        return 0;
    }
    dst = pub_bytes(o, w * hh);
    if (!dst) return 0;
    for (y = 0; y < (int)hh; y++)
        memcpy(dst + (size_t)y * w, cur + (size_t)(t + y) * s->pitch + l, w);
    return 1;
}

static int pub_seed(SURF* s)
{
    TAGPU_PUBOP* o = pub_op(PK_SEED, s->base);
    if (!o) return 0;
    o->w = s->w; o->h = s->h; o->pitch = s->pitch;
    o->l = 0; o->t = 0; o->r = (short)(s->w - 1); o->b = (short)(s->h - 1);
    if (!pub_surface_bytes(s, 0, 0, s->w - 1, s->h - 1, o)) return 0;
    pub_commit();
    s->seeded = 1;
    return 1;
}

/* THE SPRITE IDENTITY. The atlas and the seen table key a frame on its
   header and pixel-plane addresses, and the shell frees a popped screen's
   art and hands the same addresses to the next screen's: the same key would
   then name different pixels. So the key also carries a hash of the plane's
   first bytes (the row lengths and data of the first rows, up to 64 bytes),
   read here at publish time under the same guard the first-sight decode
   uses. NULL = the plane cannot be read now: the caller publishes the box's
   bytes instead, as it does when the decode fails. */
static const void* frame_key(const unsigned char* fr, const void* pix, int w, int h)
{
    const unsigned char* px = (const unsigned char*)pix;
    unsigned hh = 2166136261u, i, n = 0;
    if (!ptr_ok(px)) return NULL;
    if (fr[0x09] == 0) {                             /* raw: w*h bytes exist */
        n = (unsigned)w * (unsigned)h; if (n > 64) n = 64;
        if (IsBadReadPtr(px, n)) return NULL;
        for (i = 0; i < n; i++) hh = (hh ^ px[i]) * 16777619u;
    } else {                                         /* RLE: [len][data] per row */
        const unsigned char* q = px;
        int row;
        for (row = 0; row < h && n < 64; row++) {
            unsigned len, k;
            if (IsBadReadPtr(q, 2)) return NULL;
            len = *(const unsigned short*)q;
            if (len > 8192 || IsBadReadPtr(q, 2 + len)) return NULL;
            for (k = 0; k < 2 + len && n < 64; k++, n++) hh = (hh ^ q[k]) * 16777619u;
            q += 2 + len;
        }
    }
    hh ^= (unsigned)(size_t)pix * 2654435761u;
    hh ^= (unsigned)(unsigned short)*(const short*)(fr + 0x04) << 16;     /* the hotspot */
    hh ^= (unsigned)(unsigned short)*(const short*)(fr + 0x06);
    return (const void*)(size_t)(hh ? hh : 1u);
}

static SURF* surf_by_base(unsigned base)
{
    int i;
    for (i = 0; i < s_nsurf; i++) if (s_surf[i].base == base) return &s_surf[i];
    return NULL;
}

/* everything the census ring holds, in order, becomes published ops; the
   surface's bytes are read NOW (inside the flip, the frame complete), which
   makes a pixel op the final state of its box and the twin converge on the
   engine's surface whatever the order of the ops that wrote it */
/* THE SHELL REDRAWS EVERY GADGET ON EVERY FLIP (MEASURED 2026-09-07: ~41 ops
   per flip at ~12 000 flips a second on MAINMENU, the same with the layer on
   or off), and every one of those redraws is identical. A batch therefore
   keeps only the LAST of any run of identical ops: the final state of the
   surface is the same, because an op's replay is idempotent and the last
   occurrence is the one whose position in the order matters. The one reader
   in the op set is the COPY: dropping an earlier duplicate is safe unless a
   copy that READ its surface lies between the two with none after the
   survivor — then the replay would run the copy before the write it read.
   (A per-surface epoch bumped by every copy was tried first and defeated the
   whole dedup in the shell, whose panel is copied to the frame on every one
   of its ~12 000 flips a second: a reseed storm, 2 749 resets in one walk.) */
/* open addressing over the batch: 2x the ring's capacity keeps the load
   under a half, and a probe that runs long stops and calls the op distinct
   (a stray duplicate costs one idempotent replay, not a stall on the game
   thread inside the flip) */
#define DUP_TAB (MAX_OPS * 2)
#define DUP_PROBE_MAX 64
static int s_dupTab[DUP_TAB];
static unsigned op_hash(const OP* o)
{
    unsigned h = (unsigned)o->kind * 0x9E3779B1u;
    h ^= o->base * 0x85EBCA6Bu; h ^= (unsigned)(unsigned short)o->l * 0xC2B2AE35u; h ^= (unsigned)(unsigned short)o->t * 0x27D4EB2Fu;
    h ^= (unsigned)(unsigned short)o->r * 0x165667B1u; h ^= (unsigned)(unsigned short)o->b * 0xD3A2646Cu;
    h ^= (unsigned)(size_t)o->frame * 0xFD7046C5u; h ^= (unsigned)(size_t)o->pix * 0xB55A4F09u;
    h ^= o->src * 0x2C1B3C6Du; h ^= (unsigned)(unsigned short)o->sl * 0x297A2D39u; h ^= (unsigned)(unsigned short)o->st * 0x4F6B9E23u;
    h ^= h >> 16;
    return h & (DUP_TAB - 1);
}
static int op_same(const OP* a, const OP* b)
{
    if (!(a->kind == b->kind && a->base == b->base && a->l == b->l && a->t == b->t && a->r == b->r && a->b == b->b &&
          a->frame == b->frame && a->pix == b->pix && a->src == b->src && a->sl == b->sl && a->st == b->st))
        return 0;
    /* G17d: TWO STRINGS IN ONE BOX ARE NOT THE SAME OP. Until the string op
       existed a text draw published its box's bytes, read at publish time, so
       collapsing two draws over the same rectangle was exactly right — the
       later read carried both. A string op carries the string, so dropping the
       earlier one would drop whatever ink of it the later one does not cover. */
    if (a->kind == OP_TEXT)
        return a->slen == b->slen && a->dx == b->dx && a->dy == b->dy &&
               a->fg == b->fg && a->bg == b->bg && a->tr == b->tr &&
               (a->slen == 0 || !memcmp(s_strBuf + a->soff, s_strBuf + b->soff, a->slen));
    return 1;
}
static void dedup(void)
{
    int i;
    memset(s_dupTab, 0, sizeof s_dupTab);
    /* where the last copy that reads each surface sits in this batch */
    for (i = 0; i < s_nsurf; i++) s_surf[i].lastCopyFrom = -1;
    for (i = 0; i < s_nops; i++)
        if (s_ops[i].kind == OP_COPY) { SURF* src = surf_by_base(s_ops[i].src); if (src) src->lastCopyFrom = i; }
    for (i = 0; i < s_nops; i++) {
        OP* o = &s_ops[i];
        unsigned slot, n;
        o->dup = 0;
        if (o->kind == OP_FLIP) continue;
        slot = op_hash(o);
        for (n = 0; n < DUP_PROBE_MAX; n++, slot = (slot + 1) & (DUP_TAB - 1)) {
            if (!s_dupTab[slot]) { s_dupTab[slot] = i + 1; break; }
            if (op_same(&s_ops[s_dupTab[slot] - 1], o)) {
                int j = s_dupTab[slot] - 1;
                SURF* d = surf_by_base(o->base);
                int lc = d ? d->lastCopyFrom : -1;
                if (lc < j || lc > i) s_ops[j].dup = 1;      /* no copy read the surface between, or one follows */
                s_dupTab[slot] = i + 1;
                break;
            }
        }
    }
}

static const char* const WHY_NAME[TAGPU_GUI_WHY_N] =
    { "?", "arm", "gl-context", "queue-full", "arena-full", "box-outside-surface", "lost-sprite", "atlas-full", "untwinned-copy", "stall-over", "string-empty" };

/* THE CONSUMER CAN DIE, OR CRAWL. cnc-ddraw stops its render thread inside
   every SetDisplayMode and starts a new one with a new GL context (dd.c);
   between the two nothing drains the queue, and on the way out of a game the
   old thread presents only every few hundred ms while the game thread is in
   the exit path — and the game thread keeps flipping and this keeps
   publishing: a batch every 5 ms carrying ~150 KB of pixel bytes in game
   (the box bytes of every non-sprite op, re-read at each cadence), so the
   16 MB arena is half a second of backlog. A queue nobody reads fills; the
   overflow policy then resets and re-seeds into the full arena at every
   publish (MEASURED 2026-09-07: 24 `arena-full` resets in the 120 ms after
   the exit click, 3 643 ops queued, the tail still creeping — so a time rule
   alone never fired). So two rules say the consumer is behind: the tail has
   not moved for TAGPU_GUI_STALL_MS with work queued (dead), or the backlog
   is past half the arena or a quarter of the ring (crawling). Either way the
   batch is dropped — nothing is queued, no counter but `stalls` moves — until
   the consumer has caught up (the queue empty, or the backlog under the
   low-water marks), when one reseed brings the twins back from the surfaces
   as they are then. Returns 1 to drop. */
static unsigned arena_used(void)
{
    unsigned head = g_guiq.aHead, tail = g_guiq.aTail;     /* offsets, wrapping */
    return head >= tail ? head - tail : TAGPU_GUI_ASIZE - (tail - head);
}
static int consumer_stalled(void)
{
    static unsigned s_tailSeen = 0;
    static LARGE_INTEGER s_tailQpc, s_fq;
    static int s_stalled = 0;
    unsigned tail = g_guiq.qTail, head = g_guiq.qHead, queued = head - tail, used = arena_used();
    LARGE_INTEGER now;
    if (!s_fq.QuadPart) QueryPerformanceFrequency(&s_fq);
    QueryPerformanceCounter(&now);
    if (s_stalled) {
        if (head == tail || (queued < TAGPU_GUI_QCAP / 16 && used < TAGPU_GUI_ASIZE / 8)) {
            s_stalled = 0; s_tailSeen = tail; s_tailQpc = now;
            g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_STALL;
            return 0;
        }
        return 1;
    }
    if (queued >= TAGPU_GUI_QCAP / 4 || used >= TAGPU_GUI_ASIZE / 2) { s_stalled = 1; g_guiq.stalls++; return 1; }
    if (tail != s_tailSeen || head == tail) { s_tailSeen = tail; s_tailQpc = now; return 0; }
    if ((now.QuadPart - s_tailQpc.QuadPart) * 1000 < (LONGLONG)TAGPU_GUI_STALL_MS * s_fq.QuadPart) return 0;
    s_stalled = 1; g_guiq.stalls++;
    return 1;
}

static void publish(unsigned flipSurf)
{
    int i;
    SURF* fs;
    int vl = 0, vt = 0, vr = -1, vb = -1;
    if (!g_gui_draw) return;
    if (consumer_stalled()) return;
    dedup();
    if (g_guiq.reseed || s_pubOverflow) {
        TAGPU_PUBOP* o;
        unsigned why = g_guiq.why;
        for (i = 0; i < s_nsurf; i++) s_surf[i].seeded = 0;
        memset(s_seenF, 0, sizeof s_seenF); memset(s_seenP, 0, sizeof s_seenP);
        /* an overflow drops the queue's tail too: what the consumer has not
           taken is stale against the fresh seeds */
        if (s_pubOverflow) g_guiq.overflows++;
        g_guiq.resets++;
        g_guiq.reseed = 0; s_pubOverflow = 0; g_guiq.why = 0;
        if (s_log) {
            char b[200];
            _snprintf(b, sizeof b, "gui: reset #%u: %s (queued=%u arena=%u/%u surfaces=%d)", g_guiq.resets,
                      why < TAGPU_GUI_WHY_N ? WHY_NAME[why] : "?", g_guiq.qHead - g_guiq.qTail,
                      arena_used(), TAGPU_GUI_ASIZE, s_nsurf);
            glog(b);
        }
        o = pub_op(PK_RESET, 0);
        if (!o) return;
        pub_commit();
    }
    {
        const char* ta = *(const char* const*)TA_MAINPP;
        int L, T, W, H;
        tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
        if (W > 0 && H > 0) { vl = L; vt = T; vr = L + W - 1; vb = T + H - 1; }
    }
    fs = surf_by_base(flipSurf);
    if (fs && !fs->seeded && !pub_seed(fs)) return;
    for (i = 0; i < s_nops && !s_pubOverflow; i++) {
        OP* op = &s_ops[i];
        SURF* s;
        TAGPU_PUBOP* o;
        if (op->kind == OP_FLIP) {
            unsigned nextSeq = (i + 1 < s_nops) ? 0 : tagpu_terrown_fill_seq();
            int k;
            for (k = i + 1; k < s_nops; k++) if (s_ops[k].kind == OP_FLIP) { nextSeq = s_ops[k].seq; break; }
            if (k >= s_nops) nextSeq = tagpu_terrown_fill_seq();
            s = surf_by_base(op->base);
            if (s && !s->seeded && !pub_seed(s)) return;
            o = pub_op(PK_FRAME, op->base); if (!o) return; pub_commit();
            /* the frame that follows this flip began with the terrain skip's
               key fill: that is the viewport's erase, mirrored as a clear */
            if (nextSeq != op->seq && vr >= 0 && s) {
                o = pub_op(PK_CLEAR, op->base); if (!o) return;
                o->l = (short)vl; o->t = (short)vt; o->r = (short)vr; o->b = (short)vb;
                pub_commit();
            }
            continue;
        }
        if (op->dup) continue;
        s = surf_by_base(op->base);
        if (!s) continue;
        if (s_probeX >= 0 && s->base == flipSurf && op->l <= s_probeX && s_probeX <= op->r && op->t <= s_probeY && s_probeY <= op->b) {
            char b[300];
            const unsigned char* fr = (const unsigned char*)op->frame;
            _snprintf(b, sizeof b, "gui probe: %s box=(%d,%d)-(%d,%d) at (%d,%d) frame=%08X %ux%u ck=%u comp=%u sub=%u/%u src=%08X (%d,%d)",
                      OP_NAME[op->kind], op->l, op->t, op->r, op->b, op->dx, op->dy, (unsigned)(size_t)op->frame,
                      (unsigned)op->fw, (unsigned)op->fh, (unsigned)op->ck,
                      ptr_ok(fr) ? fr[0x09] : 0u, ptr_ok(fr) ? fr[0x0A] : 0u, ptr_ok(fr) ? fr[0x0B] : 0u,
                      op->src, op->sl, op->st);
            glog(b);
        }
        if (!s->seeded && !pub_seed(s)) return;
        /* a plain keyed blit of a frame the atlas can hold is a sprite; a frame
           past the decoder's edge (TAGPU_GAF_DECMAX, the shell's 640-wide title
           art) is its box's bytes like everything else */
        if (op->kind == OP_GAF && op->frame && op->fw && op->fh &&
            op->fw <= TAGPU_GAF_DECMAX && op->fh <= TAGPU_GAF_DECMAX) {
            const void* key = frame_key((const unsigned char*)op->frame, op->pix, op->fw, op->fh);
            if (!key) goto as_pixels;                  /* the art is not readable now */
            o = pub_op(PK_SPRITE, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->sl = op->dx; o->st = op->dy; o->fw = op->fw; o->fh = op->fh; o->ck = op->ck;
            o->frame = op->frame; o->pix = key;
            if (!seen_frame(op->frame, key, 0)) {
                unsigned char* dst = pub_bytes(o, (unsigned)op->fw * op->fh);
                if (!dst) return;
                if (!tagpu_gaf_decode((const unsigned char*)op->frame, op->fw, op->fh, dst)) {
                    /* unreadable art: the box's bytes instead, exact if dull */
                    o->kind = PK_PIXELS; o->alen = 0;
                    if (!pub_surface_bytes(s, op->l, op->t, op->r, op->b, o)) return;
                } else seen_frame(op->frame, key, 1);
            }
            pub_commit();
            continue;
        }
        /* G17d: a text draw whose string we captured is a STRING op — TA's own
           glyphs, stamped by the render thread from the coverage atlas, instead
           of ~968 arena bytes of a box that has already blended with whatever
           art it was drawn onto. A text op with no string (the scratch was
           full, or the font would not read) falls through to its box's bytes,
           which is exactly what it was before this gate. */
        if (op->kind == OP_TEXT && op->slen && op->frame && !s_nostring) {
            unsigned char* dst;
            o = pub_op(PK_STRING, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->sl = op->dx; o->st = op->dy;
            o->frame = op->frame;
            o->fg = op->fg; o->bg = op->bg; o->tr = op->tr;
            dst = pub_bytes(o, (unsigned)op->slen + 1u);
            if (!dst) return;
            memcpy(dst, s_strBuf + op->soff, (size_t)op->slen);
            dst[op->slen] = 0;
            pub_commit();
            continue;
        }
    as_pixels:
        if (op->kind == OP_COPY) {
            SURF* src = surf_by_base(op->src);
            if (src && src->seeded) {
                o = pub_op(PK_COPY, s->base); if (!o) return;
                o->src = op->src; o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
                o->sl = op->sl; o->st = op->st;
                pub_commit();
                continue;
            }
        }
        /* everything else — and a copy from a source we do not twin — is its
           box's bytes as they stand now */
        o = pub_op(PK_PIXELS, s->base); if (!o) return;
        o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
        if (!pub_surface_bytes(s, op->l, op->t, op->r, op->b, o)) return;
        pub_commit();
    }
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
    /* a surface the engine freed behind the observer's back (MEM_Free, not
       SurfaceFree) may be unmapped by now: two page probes, first and last
       row, before a whole-surface read. -1 = gone, the caller drops it. */
    if (IsBadReadPtr(cur, 1) || IsBadReadPtr(cur + (size_t)(s->h - 1) * s->pitch, (size_t)s->w)) { *outUnexpl = (unsigned)-1; return; }
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
        int r, b;
        if (o->base != s->base || o->kind == OP_FLIP) continue;
        /* the box was clamped to the surface's size WHEN RECORDED; the surface
           may have been re-made smaller since (before_free zeroes the ops of a
           freed base, but a same-base re-allocation of a different size goes
           through surf_get) — never index the mask past it */
        r = o->r < s->w - 1 ? o->r : s->w - 1;
        b = o->b < s->h - 1 ? o->b : s->h - 1;
        for (y = o->t; y <= b; y++) {
            unsigned char* m = s->mask + (size_t)y * s->w;
            for (x = o->l; x <= r; x++) if (m[x] == 255) m[x] = 128;
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

static volatile int s_inFlip = 0;      /* between the flip's entry and its return */
static void* s_retStack[32];           /* hijacked returns, LIFO (alloc, flip)    */
static int   s_retDepth = 0;

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
    int hijack = 0;
    if (!s_gameTid) s_gameTid = GetCurrentThreadId();
    else if (!on_game_thread()) return 0;
    s_flips++;
    if (s_flips == 1) {
        _snprintf(b, sizeof b, "gui: first flip on thread %u (init saw %u)", (unsigned)GetCurrentThreadId(), (unsigned)s_gameTid);
        glog(b);
    }
    src = flip_source(entry_esp);
    s = surf_of_ctx(src);
    /* the marker: this flip's surface and the fill sequence as of now */
    if (s && s_nops < MAX_OPS) {
        OP* o = &s_ops[s_nops++];
        memset(o, 0, sizeof *o);
        o->kind = OP_FLIP; o->base = s->base; o->seq = tagpu_terrown_fill_seq();
    }
    /* the cursor is drawn into the back buffer INSIDE the flip and its
       background restored before it returns: nothing in between is UI */
    if (s_retDepth < 32) {
        s_retStack[s_retDepth++] = (void*)(size_t)ret;
        s_inFlip = 1;
        hijack = 1;
    }
    if (!s_census && !g_gui_draw) { s_nops = 0; s_strUsed = 0; return hijack; }
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&now);
    if (s_lastQpc.QuadPart && (now.QuadPart - s_lastQpc.QuadPart) * 1000 < (LONGLONG)CENSUS_MS * s_freq.QuadPart)
        return hijack;                              /* too soon: keep accumulating ops */
    s_lastQpc = now;
    s_censuses++;
    if (!s_census) { publish(s ? s->base : 0); s_nops = 0; s_strUsed = 0; return hijack; }
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
        if (unexpl == (unsigned)-1) { changed = unexpl = 0; }      /* the flip's own surface cannot be gone; ignore */
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
            if (u2 == (unsigned)-1) {
                if (s_log) {
                    _snprintf(b, sizeof b, "gui census: surface %08X %dx%d is unmapped — freed behind the observer, dropped", s_surf[i].base, s_surf[i].w, s_surf[i].h);
                    glog(b);
                }
                surf_drop(i); i--;
                continue;
            }
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
    publish(s ? s->base : 0);
    s_nops = 0;
    s_strUsed = 0;
    memset(s_kindCount, 0, sizeof s_kindCount);
    memset(s_nullCtx, 0, sizeof s_nullCtx);
    s_builds = 0; s_buildFlags = 0;
    return hijack;
}

static void* __cdecl after_flip(unsigned int* regs)
{
    (void)regs;
    s_inFlip = 0;
    return s_retDepth > 0 ? s_retStack[--s_retDepth] : NULL;
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
    if (tagpu_opt_read("tagpu_gui.on", buf, sizeof buf) < 0) return 0;
    s_census = strstr(buf, "census") != NULL;
    s_log    = strstr(buf, "log") != NULL;
    s_pgm    = strstr(buf, "pgm") != NULL;
    s_trace  = strstr(buf, "trace") != NULL;
    s_nostring = strstr(buf, "nostring") != NULL;
    { const char* k = strstr(buf, "key="); if (k) s_key = atoi(k + 4) & 255; }
    { const char* k = strstr(buf, "probe="); if (k) sscanf(k + 6, "%d,%d", &s_probeX, &s_probeY); }
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
    g_guiq.ops = s_qops;
    s_arena = (unsigned char*)VirtualAlloc(NULL, TAGPU_GUI_ASIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_guiq.arena = s_arena;
    if (!s_arena) { glog("gui: NOT armed — no arena"); return; }
    ok = tagpu_detour_observe(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN, before_flip, after_flip);
    n = leaves_install();
    s_installed = ok && n == LEAF_COUNT;
    _snprintf(b, sizeof b, "gui: %s flip@0x4C63A0=%d leaves=%d/%d census=%d log=%d pgm=%d key=%d (Phase E: observers on the game thread; the layer follows the trigger, tagpu_gui_surf.c)",
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
        _snprintf(b, sizeof b, "GUI flips=%u ops=%u dropped=%u changed=%u unexplained=%u surfaces=%d published=%u bytes=%u queue=%u resets=%u overflows=%u stalls=%u draw=%d",
                  s_flips, s_opsTotal, s_opsDropped, s_changedTotal, s_unexplTotal, s_nsurf,
                  s_pubOps, s_pubBytes, g_guiq.qHead - g_guiq.qTail, g_guiq.resets, g_guiq.overflows, g_guiq.stalls, g_gui_draw);
        glog(b);
        {
            int k, n = 0;
            char ops[300];
            for (k = 1; k < OP_NKIND; k++)
                if (s_kindTotal[k]) n += _snprintf(ops + n, sizeof ops - (size_t)n, "%s%s %u", n ? " " : "", OP_NAME[k], s_kindTotal[k]);
            _snprintf(b, sizeof b, "GUI kinds: %s", ops);
            glog(b);
        }
    }
}

/* ---- G17e: the TNT's minimap picture, for the render thread --------------
   Returns 1 and fills the outputs when a picture has been snapshotted since
   the last map load. `gen` changes exactly once per load, so a consumer that
   caches anything derived from these bytes drops it when the generation moves.
   The bytes are stable for the life of that generation: one writer, one write,
   and it happens inside the map loader before any frame of that map presents. */
int tagpu_gui_minimap_pic(const unsigned char** pix, int* w, int* h, unsigned* gen)
{
    unsigned g = s_mmGen;
    if (!g) return 0;
    MemoryBarrier();
    if (pix) *pix = s_mmPic;
    if (w) *w = s_mmW;
    if (h) *h = s_mmH;
    if (gen) *gen = g;
    return 1;
}
