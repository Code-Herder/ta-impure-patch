/* tagpu_wikiasset.c -- the in-game wiki's 3D assets from the player's archives. Contract, rules and
   invariants: tagpu_wikiasset.h.

   THREADS. tagpu_wikiasset_start runs on the game thread and only hands its input over;
   tagpu_wiki3d_load runs on the loader thread it starts (or the native test's main thread) and
   touches nothing but its input, its own files and the arena; tagpu_wiki3d_get and the trigger
   read the published arena, which nothing writes after the store. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hpi.h"
#include "tagpu_json.h"
#include "tagpu_pak.h"
#include "tagpu_tdf.h"
#include "tagpu_cobscan.h"
#include "tagpu_vfs.h"
#include "tagpu_wikiworld.h"
#include "tagpu_wikiasset.h"

#ifdef _WIN32
#include <windows.h>
#include "tagpu_log.h"
#include "tagpu_model3do.h"
#define LOG tagpu_logf
#else
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

/* ---- the files' own layouts -------------------------------------------------------------------
   THE 3DO FILE'S OBJECT AND PRIMITIVE, by the offsets tools/ta3do's parse_3do reads. The engine's
   Model3DONode and face are these structs rebased (tagpu_model3do.h's N_* and F_*, checked equal
   below where that header is in reach). */
#define O_VERSION   0x00
#define O_NVERT     0x04
#define O_NFACE     0x08
#define O_SEL       0x0C
#define O_X         0x10
#define O_Z         0x18
#define O_NAME      0x1C
#define O_ZERO      0x20
#define O_VERTS     0x24
#define O_FACES     0x28
#define O_SIB       0x2C
#define O_CHILD     0x30
#define O_SIZE      0x34
#define PR_COLOR     0x00
#define PR_NIDX      0x04
#define PR_ZERO      0x08
#define PR_IDX       0x0C
#define PR_TEX       0x10
#define PR_FLAGS     0x1C
#define PR_SIZE      0x20
#define PF_FLAT     0x1u
#define PF_ANIM     0x2u
#define PF_TEAM     0x4u
#define MISS_COLOR  0xD1u       /* what 0x42A207 writes into a face whose texture no bank has */
#define MAXPIECE    256         /* TAGPU_PBMAXPIECE: the readers' piece bound                  */
#define MAXNODEV    4096        /* tagpu_posebake.c's PB_MAXNODEV                              */
#define MAXFACE     512         /* the bake's face bound per piece                             */
#define NAMEMAX     256         /* a piece or texture name in a 3DO, with its terminator      */
/* THE GAF FILE: a sequence {u16 frames, u8 @2, ..., char name[32] @8, {frame, u32} @0x28 stride 8}
   and a frame {u16 w, h, i16 hotspot, u8 key @8, u8 compressed @9, u8 sub-frames @0x0A, ...,
   pixels or the sub-frame table @0x10}, 24 bytes: tagpu_gaf.h's TAGPU_SQ_* and TAGPU_GF_*. The
   sub-frame count is a byte (0x4B8CC0, DISASSEMBLED 2026-10-26). */
#define SQ_NAME     0x08
#define SQ_TAB      0x28
#define GF_SIZE     24
#define GF_SUBN     0x0A
#define GF_PIX      0x10
#define GAF_DECMAX  640         /* TAGPU_GAF_DECMAX: the largest frame edge the decoder takes */
#define TILE_BYTES  1024
#define TNT_HDR     0x30        /* through the minimap flag at +0x2C (the scout's C1)          */

#ifdef _WIN32
/* the engine-shaped fields are read by headers this file's native build cannot include */
typedef char check_node[(N_VCOUNT == O_NVERT && N_FCOUNT == O_NFACE && N_SELPRIM == O_SEL && N_OFF == O_X &&
                         N_NAME == O_NAME && N_VERTS == O_VERTS && N_FACES == O_FACES && N_SIB == O_SIB &&
                         N_CHILD == O_CHILD && FACE_STRIDE == PR_SIZE && F_VCOUNT == PR_NIDX &&
                         F_INDICES == PR_IDX && F_TEXFRAME == PR_TEX && F_FLAGS == PR_FLAGS &&
                         FF_FLAT == PF_FLAT && FF_ANIM == PF_ANIM && FF_TEAM == PF_TEAM &&
                         TAGPU_PBMAXPIECE == MAXPIECE && WIKI3D_MAXPIECE == MAXPIECE) ? 1 : -1];
#endif

/* size caps on whole reads, a sanity bound per kind of file; the stock files' largest are far
   below each (MEASURED 2026-10-26: a 3DO under 200 KB, a texture GAF under 8 MB) */
#define CAP_TEXT    (4u << 20)
#define CAP_3DO     (8u << 20)
#define CAP_GAF     (32u << 20)
#define CAP_COB     (1u << 20)
#define FEAT_MAX    1024        /* features one description may place                          */
#define SRC_MAX     16
#define TILESET_MAX 4096

static uint32_t rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void wr16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* `n` bytes at `off` lie inside a blob of `size` */
static int inside(uint32_t size, uint32_t off, uint64_t n)
{
    return off <= size && n <= (uint64_t)(size - off);
}

static uint32_t fnv(uint32_t h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 0x01000193u;
    return h;
}
#define FNV0 0x811C9DC5u

/* ---- the arena --------------------------------------------------------------------------------
   One block, written by the loader alone and bump-allocated, never freed. Offsets, not pointers,
   inside the loader: a field the engine's readers follow holds base32 + offset, which in the DLL
   is the pointer itself. Offset 0 is never handed out, so 0 means "no allocation". */
typedef struct {
    unsigned char* base;
    uint32_t       base32, cap, used, committed;
    uint32_t     (*commit)(unsigned char* base, uint32_t upto);
    int            full;
} ARENA;

static uint32_t alloc(ARENA* a, uint32_t n)
{
    const uint32_t at = (a->used + 7u) & ~7u;
    if (a->full || at > a->cap || n > a->cap - at) { a->full = 1; return 0; }
    if (at + n > a->committed) {
        const uint32_t got = a->commit ? a->commit(a->base, at + n) : 0;
        if (got < at + n) { a->full = 1; return 0; }
        a->committed = got;
    }
    a->used = at + n;
    return at;
}

static unsigned char* AT(const ARENA* a, uint32_t off) { return a->base + off; }
static uint32_t PTR(const ARENA* a, uint32_t off) { return off ? a->base32 + off : 0; }
static uint32_t OFF(const ARENA* a, uint32_t ptr) { return ptr ? ptr - a->base32 : 0; }

static uint32_t put_str(ARENA* a, const char* s, size_t n)
{
    const uint32_t at = alloc(a, (uint32_t)n + 1);
    if (at) { memcpy(AT(a, at), s, n); AT(a, at)[n] = 0; }
    return at;
}

/* ---- the report ----------------------------------------------------------------------------- */

typedef struct { char* s; size_t n, cap; int oom; } SB;

static void sb_add(SB* b, const char* s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        char* t;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(t = (char*)realloc(b->s, cap))) { b->oom = 1; return; }
        b->s = t;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_raw(SB* b, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#include <stdarg.h>
static void sb_raw(SB* b, const char* fmt, ...)
{
    char t[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_add(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

/* a JSON string of `s` as Latin-1, lower-cased when `low`; null for NULL */
static void sb_q(SB* b, const char* s, int low)
{
    char t[8];
    if (!s) { sb_add(b, "null", 4); return; }
    sb_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (low && c >= 'A' && c <= 'Z') c += 32;
        if (c == '"' || c == '\\') { t[0] = '\\'; t[1] = (char)c; sb_add(b, t, 2); }
        else if (c < 0x20 || c >= 0x7F) { snprintf(t, sizeof t, "\\u%04x", c); sb_add(b, t, 6); }
        else { t[0] = (char)c; sb_add(b, t, 1); }
    }
    sb_add(b, "\"", 1);
}

/* ---- the loader's state ----------------------------------------------------------------------- */

typedef struct {
    char     name[NAMEMAX];
    int      kind;              /* T_* */
    uint32_t seq;               /* its sequence in the arena                                   */
    int      frames;
    int      gaf;               /* the bank's index, or -1 for logos.GAF                       */
} TEX;
enum { T_PENDING, T_STATIC, T_ANIM, T_TEAM, T_MISS };

typedef struct { uint32_t face, tex; } BIND;          /* a textured face and its TEX            */

typedef struct {
    char     path[TAGPU_VFS_PATHMAX], from[64], why[200];
    int      ok;
    uint32_t root;              /* the root node's offset                                      */
    int      pieces, faces;
} MODEL;

typedef struct {
    char path[TAGPU_VFS_PATHMAX];
    int  src;
    char why[200];              /* "" when it loaded                                            */
} BANKGAF;

typedef struct {
    const WIKI3DIN* in;
    TAGPU_VFS*      vfs;
    ARENA           a;
    TEX*            tex;   int ntex, ctex;
    BIND*           bind;  int nbind, cbind;
    BANKGAF*        bank;  int nbank;
    int             banked;     /* the bank has been searched                                   */
    BANKGAF         logos;
    int             has_logos;
    int             nfeat3d;
} LOADER;

static void why_of(char* dst, size_t n, const char* fmt, const char* a)
{
    snprintf(dst, n, fmt, a);
}

/* ---- GAF ---------------------------------------------------------------------------------------
   A GAF loaded whole for a lookup; what is kept is copied out of it into the arena. */
typedef struct { const unsigned char* b; uint32_t n; int nseq; } GAF;

/* the sequence count is the signed 16-bit [+4] (0x4B8C7D); every sequence's header must lie in
   the file, so a lookup's name read is bounded */
static int gaf_open(GAF* g, const unsigned char* b, uint32_t n, char* why, size_t nwhy)
{
    int i, count;
    g->b = b;
    g->n = n;
    g->nseq = 0;
    if (n < 12) { snprintf(why, nwhy, "a GAF shorter than its header"); return 0; }
    count = (int16_t)rd16(b + 4);
    if (count < 0) count = 0;
    if (!inside(n, 12, (uint64_t)count * 4)) { snprintf(why, nwhy, "a GAF's sequence table runs past the file"); return 0; }
    for (i = 0; i < count; i++) {
        const uint32_t so = rd32(b + 12 + 4 * i);
        if (!inside(n, so, SQ_TAB)) { snprintf(why, nwhy, "a GAF's sequence %d lies past the file", i); return 0; }
    }
    g->nseq = count;
    return 1;
}

/* 0x4B8D40: the first sequence whose name equals `name` without case, or -1 */
static int gaf_find(const GAF* g, const char* name)
{
    int i;
    for (i = 0; i < g->nseq; i++) {
        const uint32_t so = rd32(g->b + 12 + 4 * i);
        if (!tagpu_hpi_stricmp(name, (const char*)g->b + so + SQ_NAME, 32)) return i;
    }
    return -1;
}

/* the frame at `fo`, in the engine's form, its pixels or its sub-frames copied; 0 with `why` */
static uint32_t gaf_frame(ARENA* a, const GAF* g, uint32_t fo, int sub, char* why, size_t nwhy)
{
    const unsigned char* f;
    uint32_t w, h, ptr, at, data = 0, len;
    unsigned subn;
    if (!inside(g->n, fo, GF_SIZE)) { snprintf(why, nwhy, "a GAF frame lies past the file"); return 0; }
    f = g->b + fo;
    w = rd16(f);
    h = rd16(f + 2);
    subn = f[GF_SUBN];
    ptr = rd32(f + GF_PIX);
    if (w > GAF_DECMAX || h > GAF_DECMAX) {
        snprintf(why, nwhy, "a GAF frame of %lux%lu, past the decoder's %d", (unsigned long)w, (unsigned long)h, GAF_DECMAX);
        return 0;
    }
    if (subn && sub) {
        snprintf(why, nwhy, "a GAF sub-frame with sub-frames of its own, which the engine does not rebase");
        return 0;
    }
    if (!(at = alloc(a, GF_SIZE))) { snprintf(why, nwhy, "the arena is full"); return 0; }
    memcpy(AT(a, at), f, GF_SIZE);
    if (subn) {
        unsigned k;
        if (!inside(g->n, ptr, (uint64_t)subn * 4)) { snprintf(why, nwhy, "a GAF frame's sub-frame table lies past the file"); return 0; }
        if (!(data = alloc(a, subn * 4u))) { snprintf(why, nwhy, "the arena is full"); return 0; }
        for (k = 0; k < subn; k++) {
            const uint32_t s = gaf_frame(a, g, rd32(g->b + ptr + 4 * k), 1, why, nwhy);
            if (!s) return 0;
            wr32(AT(a, data) + 4 * k, PTR(a, s));
        }
    } else {
        if (f[9]) {             /* TA's RLE rows: a u16 byte count, then that many bytes */
            uint32_t pos = ptr, r;
            for (r = 0; r < h; r++) {
                if (!inside(g->n, pos, 2) || !inside(g->n, pos + 2, rd16(g->b + pos))) {
                    snprintf(why, nwhy, "a GAF frame's compressed rows run past the file");
                    return 0;
                }
                pos += 2u + rd16(g->b + pos);
            }
            len = pos - ptr;
        } else {
            len = w * h;
            if (!inside(g->n, ptr, len)) { snprintf(why, nwhy, "a GAF frame's pixels lie past the file"); return 0; }
        }
        if (!(data = alloc(a, len ? len : 1))) { snprintf(why, nwhy, "the arena is full"); return 0; }
        memcpy(AT(a, data), g->b + ptr, len);
    }
    wr32(AT(a, at) + GF_PIX, PTR(a, data));
    return at;
}

/* sequence `i` in the engine's form (0x4B8C60): its 0x28-byte header as the file has it, each
   frame entry's first dword a frame of the arena and its second the file's; 0 with `why` */
static uint32_t gaf_seq(ARENA* a, const GAF* g, int i, int* frames, char* why, size_t nwhy)
{
    const uint32_t so = rd32(g->b + 12 + 4 * i);
    const unsigned n = rd16(g->b + so);
    uint32_t at;
    unsigned k;
    if (!inside(g->n, so + SQ_TAB, (uint64_t)n * 8)) { snprintf(why, nwhy, "a GAF sequence's frame table runs past the file"); return 0; }
    if (!(at = alloc(a, SQ_TAB + n * 8u))) { snprintf(why, nwhy, "the arena is full"); return 0; }
    memcpy(AT(a, at), g->b + so, SQ_TAB + n * 8u);
    for (k = 0; k < n; k++) {
        const uint32_t fr = gaf_frame(a, g, rd32(g->b + so + SQ_TAB + 8 * k), 0, why, nwhy);
        if (!fr) return 0;
        wr32(AT(a, at) + SQ_TAB + 8 * k, PTR(a, fr));
    }
    *frames = (int)n;
    return at;
}

static unsigned char* load(LOADER* L, const TAGPU_VFILE* f, uint32_t cap, char* why, size_t nwhy)
{
    return tagpu_vfs_load(L->vfs, f, cap, why, nwhy);
}

/* ---- 3DO ---------------------------------------------------------------------------------------- */

typedef struct {
    LOADER*              L;
    const unsigned char* b;
    uint32_t             n;
    uint32_t             seen[MAXPIECE];
    int                  nseen, faces;
    char*                why;
    size_t               nwhy;
} BUILD;

static int bind_add(LOADER* L, uint32_t face, const char* name)
{
    int t;
    for (t = 0; t < L->ntex; t++)
        if (!tagpu_hpi_stricmp(name, L->tex[t].name, NAMEMAX)) break;
    if (t == L->ntex) {
        if (L->ntex == L->ctex) {
            const int c = L->ctex ? L->ctex * 2 : 256;
            TEX* x = (TEX*)realloc(L->tex, (size_t)c * sizeof *x);
            if (!x) return 0;
            L->tex = x;
            L->ctex = c;
        }
        memset(&L->tex[t], 0, sizeof *L->tex);
        strncpy(L->tex[t].name, name, NAMEMAX - 1);
        L->ntex++;
    }
    if (L->nbind == L->cbind) {
        const int c = L->cbind ? L->cbind * 2 : 1024;
        BIND* x = (BIND*)realloc(L->bind, (size_t)c * sizeof *x);
        if (!x) return 0;
        L->bind = x;
        L->cbind = c;
    }
    L->bind[L->nbind].face = face;
    L->bind[L->nbind].tex = (uint32_t)t;
    L->nbind++;
    return 1;
}

/* a NUL-terminated name at `off` of the blob, at most NAMEMAX - 1 bytes; 0 when not */
static int blob_str(const BUILD* m, uint32_t off, size_t* len)
{
    size_t k;
    if (off >= m->n) return 0;
    for (k = 0; k < NAMEMAX && off + k < m->n && m->b[off + k]; k++) {}
    if (k >= NAMEMAX || off + k >= m->n) return 0;
    *len = k;
    return 1;
}

/* THE ENGINE'S FACE ORDER (0x4CB370): the selection primitive swapped with face 0 and +0x0C set to
   0; then faces 1.. bubble-sorted ascending by mean vertex Y, the 32-bit sum of the 16.16 Ys
   divided by the count truncating toward zero (cdq; idiv), adjacent pairs from 1 swapped while a
   key exceeds the next, until a pass swaps none. The key reads the piece's vertices by the face's
   indices: an index past the piece's vertices is refused here, where the engine would read past
   the array. */
static int order_faces(BUILD* m, unsigned char* node, unsigned char* faces, uint32_t nface, uint32_t nvert, const unsigned char* verts)
{
    const ARENA* a = &m->L->a;
    int32_t* key;
    uint32_t i;
    int32_t sel = (int32_t)rd32(node + O_SEL);
    unsigned char t[PR_SIZE];
    int swapped;
    if (sel != -1 && nface) {
        if (sel < 0 || (uint32_t)sel >= nface) {
            snprintf(m->why, m->nwhy, "a selection primitive %ld past the piece's %lu faces", (long)sel, (unsigned long)nface);
            return 0;
        }
        memcpy(t, faces + PR_SIZE * (uint32_t)sel, PR_SIZE);
        memcpy(faces + PR_SIZE * (uint32_t)sel, faces, PR_SIZE);
        memcpy(faces, t, PR_SIZE);
        wr32(node + O_SEL, 0);
    }
    if (nface <= 2) return 1;
    if (!(key = (int32_t*)malloc(nface * sizeof *key))) { snprintf(m->why, m->nwhy, "out of memory"); return 0; }
    for (i = 1; i < nface; i++) {
        const unsigned char* f = faces + PR_SIZE * i;
        const uint32_t nv = rd32(f + PR_NIDX);
        const unsigned char* idx = AT(a, OFF(a, rd32(f + PR_IDX)));
        uint32_t s = 0, k;
        for (k = 0; k < nv; k++) {
            const uint32_t v = rd16(idx + 2 * k);
            if (v >= nvert) {
                free(key);
                snprintf(m->why, m->nwhy, "a face indexes vertex %lu of a piece with %lu", (unsigned long)v, (unsigned long)nvert);
                return 0;
            }
            s += rd32(verts + 12 * v + 4);
        }
        key[i] = (int32_t)s / (int32_t)nv;
    }
    do {
        swapped = 0;
        for (i = 1; i + 1 < nface; i++)
            if (key[i] > key[i + 1]) {
                const int32_t k = key[i];
                key[i] = key[i + 1];
                key[i + 1] = k;
                memcpy(t, faces + PR_SIZE * i, PR_SIZE);
                memcpy(faces + PR_SIZE * i, faces + PR_SIZE * (i + 1), PR_SIZE);
                memcpy(faces + PR_SIZE * (i + 1), t, PR_SIZE);
                swapped = 1;
            }
    } while (swapped);
    free(key);
    return 1;
}

/* the node at `off` of the blob and, recursively, its child and its sibling, into the arena in
   the engine's form; 0 with `why`. Every count and offset is bounded against the blob before it
   is read through; a node reached twice (a cycle, or two parents sharing one) or past MAXPIECE
   nodes is refused. */
static uint32_t build_node(BUILD* m, uint32_t off)
{
    ARENA* a = &m->L->a;
    const unsigned char* o;
    unsigned char* node;
    uint32_t at, nvert, nface, vo, fo, i, verts = 0, faces = 0, sib, child;
    size_t len;
    int k;
    for (k = 0; k < m->nseen; k++)
        if (m->seen[k] == off) { snprintf(m->why, m->nwhy, "the piece at 0x%lx is reached twice", (unsigned long)off); return 0; }
    if (m->nseen >= MAXPIECE) { snprintf(m->why, m->nwhy, "more than %d pieces", MAXPIECE); return 0; }
    m->seen[m->nseen++] = off;
    if (!inside(m->n, off, O_SIZE)) { snprintf(m->why, m->nwhy, "a piece at 0x%lx lies past the file", (unsigned long)off); return 0; }
    o = m->b + off;
    if (rd32(o + O_VERSION) != 1) { snprintf(m->why, m->nwhy, "a piece of version %lu, not 1", (unsigned long)rd32(o)); return 0; }
    nvert = rd32(o + O_NVERT);
    nface = rd32(o + O_NFACE);
    vo = rd32(o + O_VERTS);
    fo = rd32(o + O_FACES);
    if (nvert > MAXNODEV || nface > MAXFACE) {
        snprintf(m->why, m->nwhy, "a piece of %lu vertices and %lu faces, past %d and %d", (unsigned long)nvert, (unsigned long)nface, MAXNODEV, MAXFACE);
        return 0;
    }
    if (!inside(m->n, vo, (uint64_t)nvert * 12) || !inside(m->n, fo, (uint64_t)nface * PR_SIZE)) {
        snprintf(m->why, m->nwhy, "a piece's vertices or faces lie past the file");
        return 0;
    }
    if (!(at = alloc(a, O_SIZE))) goto full;
    node = AT(a, at);
    memcpy(node, o, O_SIZE);
    /* X and Z negated (0x4CB590), the offset's and every vertex's */
    wr32(node + O_X, 0u - rd32(o + O_X));
    wr32(node + O_Z, 0u - rd32(o + O_Z));
    if (rd32(o + O_NAME)) {
        uint32_t s;
        if (!blob_str(m, rd32(o + O_NAME), &len)) { snprintf(m->why, m->nwhy, "a piece's name is not a string within the file"); return 0; }
        if (!(s = put_str(a, (const char*)m->b + rd32(o + O_NAME), len))) goto full;
        wr32(AT(a, at) + O_NAME, PTR(a, s));
    }
    wr32(AT(a, at) + O_ZERO, 0);    /* the engine rebases it when it is not 0; nothing reads it [INFERRED] */
    if (!(verts = alloc(a, nvert ? nvert * 12u : 4u))) goto full;
    for (i = 0; i < nvert; i++) {
        const unsigned char* v = m->b + vo + 12 * i;
        unsigned char* d = AT(a, verts) + 12 * i;
        wr32(d, 0u - rd32(v));
        wr32(d + 4, rd32(v + 4));
        wr32(d + 8, 0u - rd32(v + 8));
    }
    if (!(faces = alloc(a, nface ? nface * PR_SIZE : 4u))) goto full;
    for (i = 0; i < nface; i++) {
        const unsigned char* p = m->b + fo + PR_SIZE * i;
        unsigned char* d;
        const uint32_t nv = rd32(p + PR_NIDX), io = rd32(p + PR_IDX), to = rd32(p + PR_TEX);
        uint32_t idx;
        if (!nv) { snprintf(m->why, m->nwhy, "a face with no vertices, which the engine's sort divides by"); return 0; }
        if (!inside(m->n, io, (uint64_t)nv * 2)) { snprintf(m->why, m->nwhy, "a face's vertex indices lie past the file"); return 0; }
        if (!(idx = alloc(a, nv * 2u))) goto full;
        memcpy(AT(a, idx), m->b + io, nv * 2u);
        d = AT(a, faces) + PR_SIZE * i;
        memcpy(d, p, PR_SIZE);
        wr32(d + PR_ZERO, 0);        /* as O_ZERO */
        wr32(d + PR_IDX, PTR(a, idx));
        if (to) {
            uint32_t s;
            if (!blob_str(m, to, &len)) { snprintf(m->why, m->nwhy, "a face's texture name is not a string within the file"); return 0; }
            if (!(s = put_str(a, (const char*)m->b + to, len))) goto full;
            wr32(AT(a, faces) + PR_SIZE * i + PR_TEX, PTR(a, s));
        }
    }
    node = AT(a, at);
    wr32(node + O_VERTS, PTR(a, verts));
    wr32(node + O_FACES, PTR(a, faces));
    if (!order_faces(m, node, AT(a, faces), nface, nvert, AT(a, verts))) return 0;
    /* the textured faces, at their final places, to bind once every model is in */
    for (i = 0; i < nface; i++) {
        const uint32_t t = rd32(AT(a, faces) + PR_SIZE * i + PR_TEX);
        if (t && !bind_add(m->L, faces + PR_SIZE * i, (const char*)AT(a, OFF(a, t)))) { snprintf(m->why, m->nwhy, "out of memory"); return 0; }
    }
    m->faces += (int)nface;
    sib = rd32(o + O_SIB);
    child = rd32(o + O_CHILD);
    if (child) {
        const uint32_t c = build_node(m, child);
        if (!c) return 0;
        wr32(AT(a, at) + O_CHILD, PTR(a, c));
    }
    if (sib) {
        const uint32_t s = build_node(m, sib);
        if (!s) return 0;
        wr32(AT(a, at) + O_SIB, PTR(a, s));
    }
    return at;
full:
    snprintf(m->why, m->nwhy, "the arena is full");
    return 0;
}

/* `blob` of `n` bytes as the engine holds a 3DO, its textures bound later (bind_all); 0 with why */
static uint32_t build_model(LOADER* L, const unsigned char* blob, uint32_t n, int* pieces, int* nfaces, char* why, size_t nwhy)
{
    BUILD m;
    const int nbind = L->nbind;
    uint32_t root;
    memset(&m, 0, sizeof m);
    m.L = L;
    m.b = blob;
    m.n = n;
    m.why = why;
    m.nwhy = nwhy;
    if (!(root = build_node(&m, 0))) {
        L->nbind = nbind;       /* a refused model binds nothing */
        return 0;
    }
    *pieces = m.nseen;
    *nfaces = m.faces;
    return root;
}

static int model_load(LOADER* L, MODEL* md, const char* dir, const char* name)
{
    TAGPU_VFILE f;
    unsigned char* blob;
    md->ok = 0;
    md->root = 0;
    md->why[0] = 0;
    if (!tagpu_vfs_find_built(L->vfs, dir, name, "3DO", &f)) {
        snprintf(md->path, sizeof md->path, "%s\\%s.3do", dir, name);
        why_of(md->why, sizeof md->why, "no source holds %s", md->path);
        return 0;
    }
    memcpy(md->path, f.path, sizeof md->path);
    strncpy(md->from, tagpu_vfs_source(L->vfs, f.src), sizeof md->from - 1);
    if (!(blob = load(L, &f, CAP_3DO, md->why, sizeof md->why))) return 0;
    md->root = build_model(L, blob, f.size, &md->pieces, &md->faces, md->why, sizeof md->why);
    free(blob);
    md->ok = md->root != 0;
    return md->ok;
}

/* ---- the texture bank (0x42A440, 0x4318C0) and the binder (0x42A140) ------------------------------ */

typedef struct { LOADER* L; int listed; } BANKWALK;

static int bank_each(void* ctx, const TAGPU_VFILE* f)
{
    BANKWALK* w = (BANKWALK*)ctx;
    LOADER* L = w->L;
    const char* base = strrchr(f->path, '\\');
    base = base ? base + 1 : f->path;
    w->listed++;
    if (!tagpu_hpi_stricmp(L->in->names.logos_gaf, base, TAGPU_VFS_PATHMAX)) return 1;    /* the second bank */
    if (L->nbank < 4096) {
        BANKGAF* g = (BANKGAF*)realloc(L->bank, (size_t)(L->nbank + 1) * sizeof *g);
        if (!g) return 0;
        L->bank = g;
        memset(&g[L->nbank], 0, sizeof *g);
        memcpy(g[L->nbank].path, base, strlen(base) + 1);   /* the name; opened by the builder */
        L->nbank++;
    }
    return 1;
}

/* search one GAF for every texture still pending, copying the sequences it holds */
static void bank_search(LOADER* L, BANKGAF* bg, int gi, const TAGPU_VFILE* f)
{
    unsigned char* blob;
    GAF g;
    int t;
    if (!(blob = load(L, f, CAP_GAF, bg->why, sizeof bg->why))) return;
    if (!gaf_open(&g, blob, f->size, bg->why, sizeof bg->why)) { free(blob); return; }
    for (t = 0; t < L->ntex; t++) {
        TEX* x = &L->tex[t];
        int s, frames = 0;
        char why[200];
        if (x->kind != T_PENDING || (s = gaf_find(&g, x->name)) < 0) continue;
        why[0] = 0;
        if (!(x->seq = gaf_seq(&L->a, &g, s, &frames, why, sizeof why))) {
            x->kind = T_MISS;           /* bound as a miss: the log names why */
            LOG("wiki3d: the texture %s in %s was refused - %s", x->name, f->path, why);
            continue;
        }
        x->frames = frames;
        x->gaf = gi;
        if (gi < 0 && frames == 10) x->kind = T_TEAM;
        else x->kind = frames > 1 ? T_ANIM : T_STATIC;
        if (!frames) {
            x->kind = T_MISS;
            LOG("wiki3d: the texture %s in %s has no frame, which the engine would read past", x->name, f->path);
        }
    }
    free(blob);
}

static void bank_all(LOADER* L)
{
    BANKWALK w;
    int i, n, t;
    TAGPU_VFILE f;
    if (L->banked) return;
    L->banked = 1;
    w.L = L;
    w.listed = 0;
    tagpu_vfs_enum(L->vfs, L->in->names.textures, "*.GAF", 0, bank_each, &w);
    /* the bank's count is the pattern's entries less one (0x42A480), as though logos.GAF were
       always among them: without it, the last GAF loaded is never searched */
    n = w.listed - 1 < L->nbank ? w.listed - 1 : L->nbank;
    if (n < 0) n = 0;
    L->nbank = n;
    for (i = 0; i < L->nbank; i++) {
        BANKGAF* bg = &L->bank[i];
        char name[TAGPU_VFS_PATHMAX];
        memcpy(name, bg->path, sizeof name);
        if (!tagpu_vfs_find_built(L->vfs, L->in->names.textures, name, "GAF", &f)) {
            snprintf(bg->path, sizeof bg->path, "%s\\%s", L->in->names.textures, name);
            bg->src = -2;
            snprintf(bg->why, sizeof bg->why, "no source opens it");
            continue;
        }
        memcpy(bg->path, f.path, sizeof bg->path);
        bg->src = f.src;
        bank_search(L, bg, i, &f);
    }
    if (tagpu_vfs_find_built(L->vfs, L->in->names.textures, L->in->names.logos, "GAF", &f)) {
        L->has_logos = 1;
        memcpy(L->logos.path, f.path, sizeof L->logos.path);
        L->logos.src = f.src;
        bank_search(L, &L->logos, -1, &f);
    }
    for (t = 0; t < L->ntex; t++)
        if (L->tex[t].kind == T_PENDING) L->tex[t].kind = T_MISS;
}

/* 0x42A140, face by face: FF_TEAM cleared; a miss is the flat colour 0xD1 with FF_FLAT; a sequence
   of more than one frame is an anim state at +0x10 (0x4B8B30: frame 0, the first entry's second
   dword's low word at +0x12, the sequence's byte 2 at +0x14, the sequence at +0x18) with FF_ANIM,
   and of one frame its GAFFrame at +0x10 with FF_ANIM cleared. A refused or frameless sequence is
   bound as a miss, where the engine would read past it. */
static void bind_all(LOADER* L)
{
    ARENA* a = &L->a;
    int i;
    for (i = 0; i < L->nbind; i++) {
        unsigned char* f = AT(a, L->bind[i].face);
        const TEX* x = &L->tex[L->bind[i].tex];
        uint32_t flags = rd32(f + PR_FLAGS) & ~PF_TEAM;
        if (x->kind == T_MISS) {
            wr32(f + PR_COLOR, MISS_COLOR);
            wr32(f + PR_FLAGS, flags | PF_FLAT);
            continue;
        }
        if (x->kind == T_TEAM) flags |= PF_TEAM;
        if (x->frames > 1) {
            const unsigned char* s = AT(a, x->seq);
            wr16(f + PR_TEX, 0);
            wr16(f + PR_TEX + 2, rd16(s + SQ_TAB + 4));
            f[PR_TEX + 4] = s[2];
            wr32(f + 0x18, PTR(a, x->seq));
            flags |= PF_ANIM;
        } else {
            wr32(f + PR_TEX, rd32(AT(a, x->seq) + SQ_TAB));
            flags &= ~PF_ANIM;
        }
        wr32(f + PR_FLAGS, flags);
    }
}

/* ---- the model's report: its counts, its textures and the shape hash ------------------------------
   One walk of the arena's tree in pre-order (a node, its child's subtree, its sibling's), which
   tools/wiki/assetcheck.py mirrors from the file: the hash covers each node's name, counts,
   selection field, negated offset and vertices, and each face in the engine's order with its
   colour, its indices, its flags and its texture's name, so it pins the swap, the sort, the
   negation and the binding together. */
static const TEX* face_tex(const LOADER* L, uint32_t face)
{
    int lo = 0, hi = L->nbind - 1;
    while (lo <= hi) {          /* faces are bound in arena order, so the list is sorted */
        const int mid = (lo + hi) / 2;
        if (L->bind[mid].face == face) return &L->tex[L->bind[mid].tex];
        if (L->bind[mid].face < face) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void model_report(SB* b, const LOADER* L, const MODEL* md)
{
    const ARENA* a = &L->a;
    uint32_t stack[MAXPIECE * 2];
    int sp = 0, nflat = 0, nstatic = 0, nanim = 0, nteam = 0, nmiss = 0, t, first = 1;
    int* listed = NULL;
    uint32_t h = FNV0;
    sb_raw(b, "{\"path\":");
    sb_q(b, md->path, 1);
    sb_raw(b, ",\"from\":");
    sb_q(b, md->from[0] ? md->from : NULL, 1);
    sb_raw(b, ",\"ok\":%s,\"why\":", md->ok ? "true" : "false");
    sb_q(b, md->ok ? NULL : md->why, 0);
    if (!md->ok) { sb_raw(b, "}"); return; }
    listed = (int*)calloc((size_t)L->ntex + 1, sizeof *listed);
    stack[sp++] = md->root;
    while (sp) {
        const uint32_t at = stack[--sp];
        const unsigned char* node = AT(a, at);
        const uint32_t nface = rd32(node + O_NFACE), nvert = rd32(node + O_NVERT);
        const unsigned char* faces = AT(a, OFF(a, rd32(node + O_FACES)));
        const unsigned char* verts = AT(a, OFF(a, rd32(node + O_VERTS)));
        const char* name = rd32(node + O_NAME) ? (const char*)AT(a, OFF(a, rd32(node + O_NAME))) : "";
        uint32_t i;
        h = fnv(h, name, strlen(name) + 1);
        h = fnv(h, node + O_NVERT, 12);             /* the counts and the selection field */
        h = fnv(h, node + O_X, 12);
        h = fnv(h, verts, nvert * 12u);
        for (i = 0; i < nface; i++) {
            const unsigned char* f = faces + PR_SIZE * i;
            const uint32_t nv = rd32(f + PR_NIDX), flags = rd32(f + PR_FLAGS);
            const TEX* x = face_tex(L, OFF(a, rd32(node + O_FACES)) + PR_SIZE * i);
            h = fnv(h, f + PR_COLOR, 8);
            h = fnv(h, AT(a, OFF(a, rd32(f + PR_IDX))), nv * 2u);
            h = fnv(h, f + PR_FLAGS, 4);
            h = fnv(h, x ? x->name : "", x ? strlen(x->name) + 1 : 1);
            if (flags & PF_FLAT) nflat++;
            if (x) {
                if (x->kind == T_MISS) nmiss++;
                else if (flags & PF_TEAM) nteam++;
                else if (flags & PF_ANIM) nanim++;
                else nstatic++;
                if (listed && !listed[x - L->tex]) listed[x - L->tex] = 1 + first++;
            }
        }
        /* the sibling below the child, so the child's subtree comes first */
        if (rd32(node + O_SIB)) stack[sp++] = OFF(a, rd32(node + O_SIB));
        if (rd32(node + O_CHILD)) stack[sp++] = OFF(a, rd32(node + O_CHILD));
    }
    sb_raw(b, ",\"pieces\":%d,\"faces\":%d,\"flat\":%d,\"static\":%d,\"anim\":%d,\"team\":%d,\"miss\":%d,\"shape\":\"%08lx\",\"textures\":[",
           md->pieces, md->faces, nflat, nstatic, nanim, nteam, nmiss, (unsigned long)h);
    for (t = 2; listed && t <= first; t++) {
        int k;
        for (k = 0; k < L->ntex && listed[k] != t; k++) {}
        if (k == L->ntex) continue;
        {
            const TEX* x = &L->tex[k];
            const BANKGAF* g = x->kind == T_MISS ? NULL : x->gaf < 0 ? &L->logos : &L->bank[x->gaf];
            static const char* kinds[] = { "pending", "static", "anim", "team", "miss" };
            sb_raw(b, "%s{\"name\":", t > 2 ? "," : "");
            sb_q(b, x->name, 0);
            sb_raw(b, ",\"gaf\":");
            sb_q(b, g ? g->path : NULL, 1);
            sb_raw(b, ",\"from\":");
            sb_q(b, g ? tagpu_vfs_source(L->vfs, g->src) : NULL, 1);
            sb_raw(b, ",\"kind\":\"%s\",\"frames\":%d}", kinds[x->kind], x->kind == T_MISS ? 0 : x->frames);
        }
    }
    sb_raw(b, "]}");
    free(listed);
}

/* ---- features (LoadFeature 0x4224B0) -------------------------------------------------------------
   Each distinct section name of a stage is resolved once: the first TDF of the enumeration
   0x4BCB50("features", "*.tdf") that holds it (0x4224C9..0x4224F3), then its def's keys as
   LoadFeature reads them (a missing key reads 0, `autoreclaimable` 1; the scout's B1), then its art:
   `object` present makes it 3D, its model objects3d\<object>.3DO; otherwise anims\<filename>.GAF
   and its `seqname` and `seqnameshad` sequences (0x4B8D40: none when the key is empty or names no
   sequence). LoadFeature's sharing of one GAF between two defs whose `filename`s agree in their
   first 16 bytes is not followed: each def opens its own name. */

typedef struct {
    char        name[WIKI3D_NAMEMAX];
    int         found;
    char        tdf[TAGPU_VFS_PATHMAX];
    int         src;
    TAGPU_TDF*  t;
    int         sec;
    WIKI3DFEAT  res;            /* the resolved def; its cell is each placement's              */
    int         kind;           /* 0 not found, 2 a 2D feature, 3 a 3D one                      */
    char        gaf[TAGPU_VFS_PATHMAX], gaf_from[64];
    int         frames, shadow; /* the sequences' frame counts, -1 none                         */
    MODEL       md;
    char        why[200];
} FEATDEF;

typedef struct { LOADER* L; FEATDEF* d; int nd, left; TAGPU_TDF** keep; int nkeep; } FEATWALK;

static int feat_each(void* ctx, const TAGPU_VFILE* f)
{
    FEATWALK* w = (FEATWALK*)ctx;
    char why[200];
    unsigned char* text = load(w->L, f, CAP_TEXT, why, sizeof why);
    TAGPU_TDF* t;
    int i, used = 0;
    if (!text) { LOG("wiki3d: %s (%s) not read - %s", f->path, tagpu_vfs_source(w->L->vfs, f->src), why); return 1; }
    t = tagpu_tdf_parse((const char*)text, f->size);
    free(text);
    if (!t) { LOG("wiki3d: %s (%s) is past the TDF reader's bounds", f->path, tagpu_vfs_source(w->L->vfs, f->src)); return 1; }
    for (i = 0; i < w->nd; i++) {
        FEATDEF* d = &w->d[i];
        int s;
        if (d->found || (s = tagpu_tdf_section(t, 0, d->name)) < 0) continue;
        d->found = 1;
        memcpy(d->tdf, f->path, sizeof d->tdf);
        d->src = f->src;
        d->t = t;
        d->sec = s;
        used = 1;
        w->left--;
    }
    if (used) w->keep[w->nkeep++] = t;
    else tagpu_tdf_free(t);
    return w->left > 0;
}

static const char* const FEAT_BITS[] = {
    "animating", "animtrans", "shadtrans", "flamable", "geothermal", "blocking", "reclaimable",
    "autoreclaimable", "indestructible", "nodisplayinfo", "nodrawundergray" };

static void feature_resolve(LOADER* L, FEATDEF* d)
{
    WIKI3DFEAT* out = &d->res;
    const char* object;
    int i;
    d->frames = d->shadow = -1;
    if (!d->found) {
        snprintf(d->why, sizeof d->why, "no %s\\**\\*.tdf holds the section %s", L->in->names.features, d->name);
        return;
    }
    out->fx = (int)(tagpu_tdf_int(d->t, d->sec, "footprintx", 0) & 0xFFFF);
    out->fz = (int)(tagpu_tdf_int(d->t, d->sec, "footprintz", 0) & 0xFFFF);
    out->height = (int)(tagpu_tdf_int(d->t, d->sec, "height", 0) & 0xFF);
    object = tagpu_tdf_get(d->t, d->sec, "object");
    out->flags = object ? 0u : 1u;
    for (i = 0; i < (int)(sizeof FEAT_BITS / sizeof *FEAT_BITS); i++)
        if (tagpu_tdf_int(d->t, d->sec, FEAT_BITS[i], i == 7 ? 1 : 0)) out->flags |= 2u << i;
    if (!tagpu_hpi_stricmp("DragonsTeeth", d->name, WIKI3D_NAMEMAX) || !tagpu_hpi_stricmp("DragonsTeeth_Core", d->name, WIKI3D_NAMEMAX) ||
        !tagpu_hpi_stricmp("Fortification", d->name, WIKI3D_NAMEMAX) || !tagpu_hpi_stricmp("Fortification_Core", d->name, WIKI3D_NAMEMAX))
        out->flags |= 1u << 11;
    if (object) {
        char name[WIKI3D_NAMEMAX];
        d->kind = 3;
        strncpy(name, object, sizeof name - 1);
        name[sizeof name - 1] = 0;
        model_load(L, &d->md, L->in->names.objects3d, name);
        out->model = d->md.ok ? (const char*)AT(&L->a, d->md.root) : NULL;
        out->ok = d->md.ok;
        if (!out->ok) snprintf(d->why, sizeof d->why, "its model: %s", d->md.why);
    } else {
        const char* file = tagpu_tdf_get(d->t, d->sec, "filename");
        const char* seqs[2];
        TAGPU_VFILE f;
        unsigned char* blob = NULL;
        GAF g;
        int k, ok = 1;
        d->kind = 2;
        seqs[0] = tagpu_tdf_get(d->t, d->sec, "seqname");
        seqs[1] = tagpu_tdf_get(d->t, d->sec, "seqnameshad");
        if (!tagpu_vfs_find_built(L->vfs, L->in->names.anims, file ? file : "", "GAF", &f)) {
            snprintf(d->why, sizeof d->why, "no source holds its GAF %s\\%s.gaf", L->in->names.anims, file ? file : "");
            return;
        }
        memcpy(d->gaf, f.path, sizeof d->gaf);
        strncpy(d->gaf_from, tagpu_vfs_source(L->vfs, f.src), sizeof d->gaf_from - 1);
        if (!(blob = load(L, &f, CAP_GAF, d->why, sizeof d->why)) || !gaf_open(&g, blob, f.size, d->why, sizeof d->why)) {
            free(blob);
            return;
        }
        for (k = 0; k < 2 && ok; k++) {
            int s, n = 0;
            uint32_t at;
            if (!seqs[k] || !*seqs[k] || (s = gaf_find(&g, seqs[k])) < 0) continue;
            if (!(at = gaf_seq(&L->a, &g, s, &n, d->why, sizeof d->why))) { ok = 0; break; }
            if (k == 0) { out->seq = AT(&L->a, at); d->frames = n; }
            else { out->seqshad = AT(&L->a, at); d->shadow = n; }
        }
        free(blob);
        if (ok && d->frames < 1) {
            ok = 0;
            snprintf(d->why, sizeof d->why, "its seqname %s names no sequence with a frame", seqs[0] ? seqs[0] : "(none)");
        }
        out->ok = ok;
    }
}

static void feature_report(SB* b, const LOADER* L, const FEATDEF* d)
{
    sb_raw(b, ",\"tdf\":");
    sb_q(b, d->found ? d->tdf : NULL, 1);
    sb_raw(b, ",\"from\":");
    sb_q(b, d->found ? tagpu_vfs_source(L->vfs, d->src) : NULL, 1);
    if (!d->kind) sb_raw(b, ",\"kind\":null");
    else {
        sb_raw(b, ",\"kind\":\"%s\",\"footprint\":[%d,%d],\"height\":%d,\"flags\":%lu", d->kind == 3 ? "3d" : "2d",
               d->res.fx, d->res.fz, d->res.height, (unsigned long)d->res.flags);
        if (d->kind == 3) {
            sb_raw(b, ",\"model\":");
            model_report(b, L, &d->md);
        } else {
            sb_raw(b, ",\"gaf\":");
            sb_q(b, d->gaf[0] ? d->gaf : NULL, 1);
            sb_raw(b, ",\"gaf_from\":");
            sb_q(b, d->gaf_from[0] ? d->gaf_from : NULL, 1);
            if (d->frames >= 0) sb_raw(b, ",\"frames\":%d", d->frames); else sb_raw(b, ",\"frames\":null");
            if (d->shadow >= 0) sb_raw(b, ",\"shadow_frames\":%d", d->shadow); else sb_raw(b, ",\"shadow_frames\":null");
        }
    }
    sb_raw(b, ",\"ok\":%s,\"why\":", d->res.ok ? "true" : "false");
    sb_q(b, d->res.ok ? NULL : d->why, 0);
}

/* ---- stages -------------------------------------------------------------------------------------- */

typedef struct {
    char        map[TAGPU_VFS_PATHMAX];
    int         tiles, found, ok;
    TAGPU_VFILE f;
    uint32_t    gfx, count;
    char        why[200];
} SRCREC;

typedef struct { int src, index, ok; uint32_t hash; char why[200]; } TILEREC;

typedef struct {
    int      refused;           /* the description itself was refused: `why` says how          */
    char     why[200];
    int      w, h, nsrc, ntile, nplace, ndef;
    SRCREC   src[SRC_MAX];
    TILEREC* tile;
    int*     place_def;         /* each placed feature's FEATDEF                                */
    FEATDEF* def;
} STAGEREC;

static int hexval(const char* s, uint32_t* v)
{
    int i;
    *v = 0;
    for (i = 0; i < 8; i++) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        *v = *v << 4 | (uint32_t)d;
    }
    return !s[8];
}

static int tj_int(const TJV* v, int* out)
{
    double d;
    if (!tj_is(v, TJ_NUM)) return 0;
    d = tj_num(v, 0);
    if (!(d >= -2147483648.0 && d <= 2147483647.0) || d != (double)(int)d) return 0;
    *out = (int)d;
    return 1;
}

/* THE DESCRIPTION (the plan's section 4), checked before anything is read through it: its entry,
   its cells against the entry's map, each source, each tileset row's source and hash, each tilemap
   index against the tileset, each feature's cell inside the map. */
static int stage_parse(LOADER* L, const WIKIENTRY* e, const TJV* r, WIKI3DSTAGE* st, STAGEREC* s)
{
    const TJV *x, *v;
    int i, k, ntm;
    uint32_t map_at, feat_at = 0;
    const TJV* c = tj_get(r, "cells");
    char* why = s->why;
    const size_t nwhy = sizeof s->why;
    if (tj_num(tj_get(r, "format"), 0) != 1) { snprintf(why, nwhy, "not format 1"); return 0; }
    if (strcmp(tj_str(tj_get(r, "entry"), ""), e->id)) { snprintf(why, nwhy, "its entry is not %s", e->id); return 0; }
    if (!tj_int(tj_at(c, 0), &s->w) || !tj_int(tj_at(c, 1), &s->h) || s->w != e->scene.w || s->h != e->scene.h ||
        (s->w & 1) || (s->h & 1) || s->w <= 0 || s->h <= 0 || s->w > WIKI_MAP_MAX || s->h > WIKI_MAP_MAX) {
        snprintf(why, nwhy, "its cells are not the entry's map of %d x %d, even", e->scene.w, e->scene.h);
        return 0;
    }
    for (v = (x = tj_get(r, "sources")) && tj_is(x, TJ_ARR) ? x->first : NULL; v; v = v->next) {
        SRCREC* sr = &s->src[s->nsrc];
        const char* m = tj_str(tj_get(v, "map"), NULL);
        if (s->nsrc == SRC_MAX) { snprintf(why, nwhy, "more than %d sources", SRC_MAX); return 0; }
        if (!m || strlen(m) >= TAGPU_VFS_PATHMAX || !strchr(m, '\\') || !tj_int(tj_get(v, "tiles"), &sr->tiles) || sr->tiles <= 0) {
            snprintf(why, nwhy, "source %d is not {map: <dir>\\<name>, tiles}", s->nsrc);
            return 0;
        }
        strcpy(sr->map, m);
        s->nsrc++;
    }
    x = tj_get(r, "tileset");
    s->ntile = tj_is(x, TJ_ARR) ? x->n : 0;
    if (s->ntile <= 0 || s->ntile > TILESET_MAX || !(s->tile = (TILEREC*)calloc((size_t)s->ntile, sizeof *s->tile))) {
        snprintf(why, nwhy, "a tileset of %d rows", s->ntile);
        return 0;
    }
    for (i = 0, v = x->first; v; v = v->next, i++) {
        TILEREC* t = &s->tile[i];
        if (!tj_int(tj_at(v, 0), &t->src) || t->src < 0 || t->src >= s->nsrc || !tj_int(tj_at(v, 1), &t->index) ||
            t->index < 0 || !hexval(tj_str(tj_at(v, 2), ""), &t->hash)) {
            snprintf(why, nwhy, "tileset row %d is not [source, tile, hash]", i);
            return 0;
        }
    }
    x = tj_get(r, "tilemap");
    ntm = (s->w / 2) * (s->h / 2);
    if (!tj_is(x, TJ_ARR) || x->n != ntm) { snprintf(why, nwhy, "a tilemap of other than %d tiles", ntm); return 0; }
    if (!(map_at = alloc(&L->a, (uint32_t)ntm * 2u))) { snprintf(why, nwhy, "the arena is full"); return 0; }
    for (i = 0, v = x->first; v; v = v->next, i++) {
        int t;
        if (!tj_int(v, &t) || t < 0 || t >= s->ntile) { snprintf(why, nwhy, "tilemap %d is no tileset row", i); return 0; }
        wr16(AT(&L->a, map_at) + 2 * i, (uint16_t)t);
    }
    st->tilemap = (const uint16_t*)AT(&L->a, map_at);
    x = tj_get(r, "features");
    s->nplace = tj_is(x, TJ_ARR) ? x->n : 0;
    if (s->nplace > FEAT_MAX) { snprintf(why, nwhy, "%d features, past %d", s->nplace, FEAT_MAX); return 0; }
    if (s->nplace && (!(s->place_def = (int*)calloc((size_t)s->nplace, sizeof *s->place_def)) ||
                      !(s->def = (FEATDEF*)calloc((size_t)s->nplace, sizeof *s->def)) ||
                      !(feat_at = alloc(&L->a, (uint32_t)s->nplace * (uint32_t)sizeof(WIKI3DFEAT))))) {
        snprintf(why, nwhy, "out of memory for %d features", s->nplace);
        return 0;
    }
    for (i = 0, v = s->nplace ? x->first : NULL; v; v = v->next, i++) {
        WIKI3DFEAT* f = (WIKI3DFEAT*)AT(&L->a, feat_at) + i;
        const char* n = tj_str(tj_get(v, "name"), NULL);
        if (!n || !*n || strlen(n) >= WIKI3D_NAMEMAX || !tj_int(tj_at(tj_get(v, "cell"), 0), &f->cx) ||
            !tj_int(tj_at(tj_get(v, "cell"), 1), &f->cz) || f->cx < 0 || f->cz < 0 || f->cx >= s->w || f->cz >= s->h) {
            snprintf(why, nwhy, "feature %d is not {name, cell} inside the map", i);
            return 0;
        }
        strcpy(f->name, n);
        for (k = 0; k < s->ndef && tagpu_hpi_stricmp(n, s->def[k].name, WIKI3D_NAMEMAX); k++) {}
        if (k == s->ndef) strcpy(s->def[s->ndef++].name, n);
        s->place_def[i] = k;
    }
    st->feat = s->nplace ? (const WIKI3DFEAT*)AT(&L->a, feat_at) : NULL;
    st->nfeat = s->nplace;
    return 1;
}

static void stage_load(LOADER* L, const WIKIENTRY* e, WIKI3DSTAGE* st, STAGEREC* s)
{
    char name[64];
    const unsigned char* text;
    size_t ntext;
    TJDOC* doc = NULL;
    uint32_t tiles_at;
    int i, ok = 1;
    strncpy(st->entry, e->id, sizeof st->entry - 1);
    snprintf(name, sizeof name, "stage/%s.json", e->id);
    if (!(text = tagpu_pak_file(L->in->pak, L->in->npak, name, &ntext))) {
        snprintf(s->why, sizeof s->why, "WIKIPAK has no %s", name);
        s->refused = 1;
    } else if (!(doc = tagpu_json_parse((const char*)text, ntext, s->why, sizeof s->why)) ||
               !stage_parse(L, e, tagpu_json_root(doc), st, s)) {
        s->refused = 1;
    }
    tagpu_json_free(doc);
    if (s->refused) goto done;
    st->w = s->w;
    st->h = s->h;
    if (!(tiles_at = alloc(&L->a, (uint32_t)s->ntile * TILE_BYTES))) {
        snprintf(s->why, sizeof s->why, "the arena is full");
        s->refused = 1;
        goto done;
    }
    st->ntile = s->ntile;
    st->tiles = AT(&L->a, tiles_at);
    /* the ground: each source's header (the scout's C1: tile count +0x18, tiles +0x14) and count */
    for (i = 0; i < s->nsrc; i++) {
        SRCREC* sr = &s->src[i];
        char dir[TAGPU_VFS_PATHMAX];
        const char* sl = strchr(sr->map, '\\');
        const char* dot = strrchr(sl, '.');
        char base[TAGPU_VFS_PATHMAX];
        unsigned char hd[TNT_HDR];
        memcpy(dir, sr->map, (size_t)(sl - sr->map));
        dir[sl - sr->map] = 0;
        strcpy(base, sl + 1);
        /* 0x435430 builds a map's path as 0x4290F0 does: "<dir>-<language>\<name>" first */
        if (!tagpu_vfs_find_built(L->vfs, dir, base, dot ? dot + 1 : "", &sr->f))
            snprintf(sr->why, sizeof sr->why, "no source holds %s", sr->map);
        else if (sr->found = 1, !tagpu_vfs_read(L->vfs, &sr->f, 0, TNT_HDR, hd, sr->why, sizeof sr->why)) {}
        else if (rd32(hd) != 0x2000 && rd32(hd) != 0x1020)
            snprintf(sr->why, sizeof sr->why, "a TNT of version 0x%lx", (unsigned long)rd32(hd));
        else if ((sr->count = rd32(hd + 0x18)) != (uint32_t)sr->tiles)
            snprintf(sr->why, sizeof sr->why, "%lu tiles where the description expects %d", (unsigned long)sr->count, sr->tiles);
        else if (sr->gfx = rd32(hd + 0x14), !inside(sr->f.size, sr->gfx, (uint64_t)sr->count * TILE_BYTES))
            snprintf(sr->why, sizeof sr->why, "its %lu tiles at 0x%lx run past the file's %lu bytes",
                     (unsigned long)sr->count, (unsigned long)sr->gfx, (unsigned long)sr->f.size);
        else sr->ok = 1;
        if (!sr->ok) ok = 0;
    }
    /* only the tiles the description names, each hashed against it */
    for (i = 0; i < s->ntile; i++) {
        TILEREC* t = &s->tile[i];
        const SRCREC* sr = &s->src[t->src];
        unsigned char* px = AT(&L->a, tiles_at) + TILE_BYTES * i;
        uint32_t got;
        if (!sr->ok) snprintf(t->why, sizeof t->why, "its source did not open");
        else if ((uint32_t)t->index >= sr->count)
            snprintf(t->why, sizeof t->why, "tile %d past the map's %lu", t->index, (unsigned long)sr->count);
        else if (!tagpu_vfs_read(L->vfs, &sr->f, sr->gfx + (uint32_t)t->index * TILE_BYTES, TILE_BYTES, px, t->why, sizeof t->why)) {}
        else if ((got = fnv(FNV0, px, TILE_BYTES)) != t->hash)
            snprintf(t->why, sizeof t->why, "its bytes hash to %08lx, not %08lx", (unsigned long)got, (unsigned long)t->hash);
        else t->ok = 1;
        if (!t->ok) { ok = 0; memset(px, 0, TILE_BYTES); }
    }
    /* the features: each distinct section from the first TDF holding it, then its art */
    if (s->ndef) {
        FEATWALK fw;
        TAGPU_TDF** keep = (TAGPU_TDF**)calloc((size_t)s->ndef, sizeof *keep);
        fw.L = L; fw.d = s->def; fw.nd = s->ndef; fw.left = s->ndef; fw.keep = keep; fw.nkeep = 0;
        if (keep) tagpu_vfs_enum(L->vfs, L->in->names.features, "*.tdf", 1, feat_each, &fw);
        for (i = 0; i < s->ndef; i++) {
            feature_resolve(L, &s->def[i]);
            if (s->def[i].kind == 3) st->nfeat3d++;
        }
        for (i = 0; keep && i < fw.nkeep; i++) tagpu_tdf_free(keep[i]);
        free(keep);
        for (i = 0; i < s->ndef; i++) s->def[i].t = NULL;
    }
    for (i = 0; i < s->nplace; i++) {
        WIKI3DFEAT* f = (WIKI3DFEAT*)st->feat + i;
        const FEATDEF* d = &s->def[s->place_def[i]];
        const int cx = f->cx, cz = f->cz;
        char nm[WIKI3D_NAMEMAX];
        memcpy(nm, f->name, sizeof nm);
        *f = d->res;
        f->cx = cx;
        f->cz = cz;
        memcpy(f->name, nm, sizeof nm);
        if (!f->ok) ok = 0;
    }
    if (st->nfeat3d > (int)WIKIWORLD_FEAT3D) {
        snprintf(s->why, sizeof s->why, "%d 3D features, past the stage's %u", st->nfeat3d, (unsigned)WIKIWORLD_FEAT3D);
        st->ok = 0;
        goto done;
    }
    st->ok = ok;
    if (!ok) snprintf(s->why, sizeof s->why, "a source, tile or feature did not resolve: the ground falls back whole");
done:
    if (!st->ok) LOG("wiki3d: the stage %s falls back to the whitebox - %s", e->id, s->why);
}

static void stage_report(SB* b, const LOADER* L, const WIKI3DSTAGE* st, const STAGEREC* s)
{
    int i;
    sb_raw(b, "{\"entry\":");
    sb_q(b, st->entry, 0);
    if (s->refused) {
        sb_raw(b, ",\"ok\":false,\"why\":");
        sb_q(b, s->why, 0);
        sb_raw(b, "}");
        return;
    }
    sb_raw(b, ",\"cells\":[%d,%d],\"sources\":[", s->w, s->h);
    for (i = 0; i < s->nsrc; i++) {
        const SRCREC* sr = &s->src[i];
        sb_raw(b, "%s{\"map\":", i ? "," : "");
        sb_q(b, sr->map, 1);
        sb_raw(b, ",\"path\":");
        sb_q(b, sr->found ? sr->f.path : NULL, 1);
        sb_raw(b, ",\"from\":");
        sb_q(b, sr->found ? tagpu_vfs_source(L->vfs, sr->f.src) : NULL, 1);
        sb_raw(b, ",\"tiles\":%d,\"ok\":%s,\"why\":", sr->tiles, sr->ok ? "true" : "false");
        sb_q(b, sr->ok ? NULL : sr->why, 0);
        sb_raw(b, "}");
    }
    sb_raw(b, "],\"tiles\":[");
    for (i = 0; i < s->ntile; i++) {
        const TILEREC* t = &s->tile[i];
        sb_raw(b, "%s{\"source\":%d,\"index\":%d,\"hash\":\"%08lx\",\"ok\":%s,\"why\":", i ? "," : "", t->src, t->index,
               (unsigned long)t->hash, t->ok ? "true" : "false");
        sb_q(b, t->ok ? NULL : t->why, 0);
        sb_raw(b, "}");
    }
    sb_raw(b, "],\"features\":[");
    for (i = 0; i < s->nplace; i++) {
        sb_raw(b, "%s{\"name\":", i ? "," : "");
        sb_q(b, st->feat[i].name, 0);
        sb_raw(b, ",\"cell\":[%d,%d]", st->feat[i].cx, st->feat[i].cz);
        feature_report(b, L, &s->def[s->place_def[i]]);
        sb_raw(b, "}");
    }
    sb_raw(b, "],\"features_3d\":%d,\"ok\":%s,\"why\":", st->nfeat3d, st->ok ? "true" : "false");
    sb_q(b, st->ok ? NULL : s->why, 0);
    sb_raw(b, "}");
}

static void stage_free(STAGEREC* s)
{
    free(s->tile);
    free(s->place_def);
    free(s->def);
}

/* ---- units --------------------------------------------------------------------------------------- */

typedef struct {
    int         found, script, info;   /* info: the FBI's [UNITINFO] was read                  */
    TAGPU_VFILE f, sf;
    char        unitname[WIKI3D_NAMEMAX], object[WIKI3D_NAMEMAX];
    MODEL       md;
    char        hides_why[64];  /* "" unless the mask was refused                               */
    char        uncached_why[64];
    char        why[200];
} UNITREC;

/* THE TWO PIECE ORDERS the mask joins, over a model of this arena: tagpu_model3do.h's
   tagpu_model_walk (the readers' walk, the order of the bits) and tagpu_model_engine_order (the
   engine's order for a new unit, the order COB pieces name). In the DLL the arena's fields are
   pointers, so those two functions walk it themselves; the native build, whose arena holds
   stand-in addresses, walks the same fields by offset with the same rules, and the game's
   report equalling the native one (tools/wiki_regression.py `asset`) holds the two equal. A
   model of this arena was built with every node reached once and at most MAXPIECE of them, so
   both walks end. Host pointers come out, the nodes' and their names'. Returns the engine order's
   count with the walk's in *np, or -1 for a walk past MAXPIECE. */
#ifdef _WIN32
static int piece_orders(const ARENA* a, uint32_t root, const char** walk, int* np, const char** prim,
                        const char** pname)
{
    const int nprim = tagpu_model_engine_order((const char*)AT(a, root), prim, pname);
    *np = tagpu_model_walk((const char*)AT(a, root), walk, MAXPIECE);
    return *np > 0 && nprim > 0 ? nprim : -1;
}
#else
static uint32_t field(const ARENA* a, uint32_t node, uint32_t at) { return OFF(a, rd32(AT(a, node + at))); }

static int piece_orders(const ARENA* a, uint32_t root, const char** walk, int* npp, const char** prim,
                        const char** pname)
{
    uint32_t stack[MAXPIECE + 2];
    int sp = 0, np = 0, nprim = 0;
    /* tagpu_model_walk: a node, then its children pushed in sibling order; the root's siblings not */
    stack[sp++] = root;
    while (sp > 0) {
        const uint32_t nd = stack[--sp];
        uint32_t ch;
        if (np >= MAXPIECE) return -1;
        walk[np++] = (const char*)AT(a, nd);
        for (ch = field(a, nd, O_CHILD); ch; ch = field(a, ch, O_SIB)) {
            if (sp >= MAXPIECE) return -1;
            stack[sp++] = ch;
        }
    }
    /* tagpu_model_engine_order: a node, its child's subtree, then its sibling's, the root's too */
    stack[sp++] = root;
    while (sp > 0) {
        const uint32_t nd = stack[--sp], sib = field(a, nd, O_SIB), ch = field(a, nd, O_CHILD);
        const uint32_t nm = field(a, nd, O_NAME);
        if (nprim >= MAXPIECE) return -1;
        pname[nprim] = nm ? (const char*)AT(a, nm) : NULL;
        prim[nprim++] = (const char*)AT(a, nd);
        if (sib) stack[sp++] = sib;
        if (ch) stack[sp++] = ch;
        if (sp > MAXPIECE) return -1;
    }
    *npp = np;
    return np > 0 && nprim > 0 ? nprim : -1;
}
#endif

/* CREATE()'S HIDES (`what` TAGPU_COB_HIDE) OR DONT-CACHES (TAGPU_COB_DONTCACHE):
   tagpu_cobscan_create_mask, the rule the build ghost's mask takes (tagpu_datakeys.c), over the
   script's file bytes (base 0: a file's offsets are unrelocated) and this model. The engine's
   order holds the root's siblings and the readers' walk does not; the mask names nodes by
   address, so a marked sibling of the root is simply not in the bits. */
static int unit_hides(const ARENA* a, const unsigned char* cob, uint32_t n, uint32_t root, int what,
                      uint32_t* bits, char* why, size_t nwhy)
{
    const char* walk[MAXPIECE];
    const char* prim[MAXPIECE];
    const char* pname[MAXPIECE];
    int np = 0, r;
    const int nprim = piece_orders(a, root, walk, &np, prim, pname);
    memset(bits, 0, MAXPIECE / 8);
    if (nprim < 0) { snprintf(why, nwhy, "the model's walk past %d pieces", MAXPIECE); return -1; }
    r = tagpu_cobscan_create_mask((const char*)cob, n, 0, what, prim, pname, nprim, walk, np, bits);
    if (r < 0) {
        snprintf(why, nwhy, r == -2 ? "an empty script" : "the script or the model refused");
        memset(bits, 0, MAXPIECE / 8);
        return -1;
    }
    return r;
}

/* the report's "hides" (and "uncached"): null without a model and a script; else the marked
   nodes' names in the readers' walk, the bits as words (word 0 first), and the refusal */
static void unit_hides_report(SB* b, const LOADER* L, const WIKI3DUNIT* u, const UNITREC* r,
                              const uint32_t* bits, int state, const char* why)
{
    const char* walk[MAXPIECE];
    const char* prim[MAXPIECE];
    const char* pname[MAXPIECE];
    int np = 0, g, first = 1;
    if (!u->model || !r->script) { sb_raw(b, "null"); return; }
    if (piece_orders(&L->a, r->md.root, walk, &np, prim, pname) < 0) np = 0;
    sb_raw(b, "{\"pieces\":[");
    for (g = 0; g < np; g++)
        if (bits[g >> 5] >> (g & 31) & 1u) {
            const uint32_t nm = OFF(&L->a, rd32((const unsigned char*)walk[g] + O_NAME));
            sb_raw(b, first ? "" : ",");
            sb_q(b, nm ? (const char*)AT(&L->a, nm) : "", 0);
            first = 0;
        }
    sb_raw(b, "],\"bits\":\"");
    for (g = 0; g < MAXPIECE / 32; g++) sb_raw(b, "%08lx", (unsigned long)bits[g]);
    sb_raw(b, "\",\"why\":");
    sb_q(b, state < 0 ? why : NULL, 0);
    sb_raw(b, "}");
}

static void unit_load(LOADER* L, const char* type, WIKI3DUNIT* u, UNITREC* r)
{
    unsigned char* text;
    TAGPU_TDF* t = NULL;
    int sec;
    strncpy(u->name, type, sizeof u->name - 1);
    /* a level's rule (0x42D709): units\<UnitName>.FBI by path, the language's folder first */
    if (!tagpu_vfs_find_built(L->vfs, L->in->names.units, type, "FBI", &r->f)) {
        snprintf(r->why, sizeof r->why, "no source holds %s\\%s.fbi", L->in->names.units, type);
    } else if (r->found = 1, !(text = load(L, &r->f, CAP_TEXT, r->why, sizeof r->why))) {
    } else {
        const char *un, *on;
        t = tagpu_tdf_parse((const char*)text, r->f.size);
        free(text);
        if (!t || (sec = tagpu_tdf_section(t, 0, "UNITINFO")) < 0) {
            snprintf(r->why, sizeof r->why, "%s has no [UNITINFO]", r->f.path);
        } else {
            /* 0x42BF40 reads UnitName and Objectname into 0x20 bytes each; a missing Objectname is
               the UnitName (0x42C0D1) */
            un = tagpu_tdf_get(t, sec, "UnitName");
            on = tagpu_tdf_get(t, sec, "Objectname");
            strncpy(r->unitname, un ? un : "", sizeof r->unitname - 1);
            strncpy(r->object, on ? on : r->unitname, sizeof r->object - 1);
            /* the facts the stage draws it by (WIKI3DUNIT) */
            r->info = 1;
            u->mobile = tagpu_tdf_int(t, sec, "BMcode", 1) != 0;
            u->slant = !u->mobile && tagpu_tdf_int(t, sec, "Digger", 0) == 0;
            u->shadow = tagpu_tdf_int(t, sec, "NoShadow", 0) == 0 &&
                        (u->slant || (tagpu_tdf_int(t, sec, "CanHover", 0) == 0 &&
                                      tagpu_tdf_int(t, sec, "Floater", 0) == 0));
            if (tagpu_hpi_stricmp(type, r->unitname, sizeof r->unitname))
                snprintf(r->why, sizeof r->why, "%s names the UnitName %s, not %s", r->f.path, r->unitname, type);
            else if (model_load(L, &r->md, L->in->names.objects3d, r->object)) {
                u->model = (const char*)AT(&L->a, r->md.root);
                u->ok = 1;
            } else snprintf(r->why, sizeof r->why, "its model: %s", r->md.why);
        }
    }
    tagpu_tdf_free(t);
    /* the script by UnitName, never Objectname (0x42D8E5) */
    if (r->unitname[0] && tagpu_vfs_find_built(L->vfs, L->in->names.scripts, r->unitname, "COB", &r->sf)) {
        char sw[200];
        unsigned char* cob = load(L, &r->sf, CAP_COB, sw, sizeof sw);
        r->script = 1;
        if (cob && u->model) {
            u->hides = unit_hides(&L->a, cob, r->sf.size, r->md.root, TAGPU_COB_HIDE, u->hidden,
                                  r->hides_why, sizeof r->hides_why);
            u->uncaches = unit_hides(&L->a, cob, r->sf.size, r->md.root, TAGPU_COB_DONTCACHE, u->uncached,
                                     r->uncached_why, sizeof r->uncached_why);
        }
        free(cob);
    }
    if (!u->ok) LOG("wiki3d: the unit type %s stays a whitebox box - %s", type, r->why);
}

static void unit_report(SB* b, const LOADER* L, const WIKI3DUNIT* u, const UNITREC* r)
{
    sb_raw(b, "{\"name\":");
    sb_q(b, u->name, 0);
    sb_raw(b, ",\"fbi\":");
    sb_q(b, r->found ? r->f.path : NULL, 1);
    sb_raw(b, ",\"from\":");
    sb_q(b, r->found ? tagpu_vfs_source(L->vfs, r->f.src) : NULL, 1);
    sb_raw(b, ",\"unitname\":");
    sb_q(b, r->unitname[0] ? r->unitname : NULL, 0);
    sb_raw(b, ",\"object\":");
    sb_q(b, r->object[0] ? r->object : NULL, 1);
    sb_raw(b, ",\"model\":");
    if (r->md.path[0]) model_report(b, L, &r->md); else sb_raw(b, "null");
    sb_raw(b, ",\"script\":");
    sb_q(b, r->script ? r->sf.path : NULL, 1);
    sb_raw(b, ",\"script_from\":");
    sb_q(b, r->script ? tagpu_vfs_source(L->vfs, r->sf.src) : NULL, 1);
    sb_raw(b, ",\"hides\":");
    unit_hides_report(b, L, u, r, u->hidden, u->hides, r->hides_why);
    sb_raw(b, ",\"uncached\":");
    unit_hides_report(b, L, u, r, u->uncached, u->uncaches, r->uncached_why);
    if (r->info)
        sb_raw(b, ",\"facts\":{\"mobile\":%s,\"slant\":%s,\"shadow\":%s}", u->mobile ? "true" : "false",
               u->slant ? "true" : "false", u->shadow ? "true" : "false");
    else
        sb_raw(b, ",\"facts\":null");
    sb_raw(b, ",\"ok\":%s,\"why\":", u->ok ? "true" : "false");
    sb_q(b, u->ok ? NULL : r->why, 0);
    sb_raw(b, "}");
}

/* ---- the load ----------------------------------------------------------------------------------- */

#define REPORT_RESERVE (1u << 20)   /* the arena's tail the report alone may use: assets that
                                       fill the rest are refused, and the report still fits      */

const WIKI3D* tagpu_wiki3d_load(const WIKI3DIN* in, unsigned char* base, uint32_t base32, uint32_t cap,
                                uint32_t (*commit)(unsigned char* base, uint32_t upto))
{
    LOADER L;
    SB b;
    WIKI3D* w;
    STAGEREC* srec = NULL;
    UNITREC* urec = NULL;
    uint32_t w_at, st_at = 0, un_at = 0, rep_at, assets;
    const WIKIDATA* d = in->data;
    char (*names)[WIKI_ID_MAX] = NULL;
    int i, k, n = 0, nunit = 0, nstage = d ? d->nentry : 0;
    memset(&L, 0, sizeof L);
    memset(&b, 0, sizeof b);
    L.in = in;
    L.a.base = base;
    L.a.base32 = base32;
    L.a.cap = cap > REPORT_RESERVE ? cap - REPORT_RESERVE : 0;
    L.a.used = 8;
    L.a.commit = commit;
    L.a.committed = commit ? 0 : cap;
    if (!(w_at = alloc(&L.a, sizeof *w))) return NULL;
    if (nstage && (!(st_at = alloc(&L.a, (uint32_t)(nstage * sizeof(WIKI3DSTAGE)))) ||
                   !(srec = (STAGEREC*)calloc((size_t)nstage, sizeof *srec))))
        nstage = 0;
    /* the unit types every entry defines, each once, in the entries' order */
    for (i = 0; i < nstage; i++) nunit += d->entry[i].ndef;
    if (nunit && !(names = (char (*)[WIKI_ID_MAX])calloc((size_t)nunit, sizeof *names))) nunit = 0;
    for (i = 0; names && i < nstage; i++)
        for (k = 0; k < d->entry[i].ndef; k++) {
            int j;
            for (j = 0; j < n && strcmp(names[j], d->entry[i].def[k].name); j++) {}
            if (j == n) memcpy(names[n++], d->entry[i].def[k].name, WIKI_ID_MAX);
        }
    nunit = names ? n : 0;
    if (nunit && (!(un_at = alloc(&L.a, (uint32_t)(nunit * sizeof(WIKI3DUNIT)))) ||
                  !(urec = (UNITREC*)calloc((size_t)nunit, sizeof *urec))))
        nunit = 0;
    L.vfs = tagpu_vfs_open(in->cwd, (const char (*)[TAGPU_VFS_PATHMAX])in->arc, in->narc, in->names.language);
    if (L.vfs) {
        for (i = 0; i < nstage; i++) stage_load(&L, &d->entry[i], (WIKI3DSTAGE*)AT(&L.a, st_at) + i, &srec[i]);
        for (i = 0; i < nunit; i++) unit_load(&L, names[i], (WIKI3DUNIT*)AT(&L.a, un_at) + i, &urec[i]);
        /* every model is in: the bank is searched once for every name they hold, then bound */
        bank_all(&L);
        bind_all(&L);
    }
    assets = L.a.used;
    /* THE REPORT, once everything is bound */
    sb_raw(&b, "{\"format\":1,\"language\":");
    sb_q(&b, in->names.language, 0);
    sb_raw(&b, ",\"names\":{\"units\":");
    sb_q(&b, in->names.units, 0);
    sb_raw(&b, ",\"textures\":");
    sb_q(&b, in->names.textures, 0);
    sb_raw(&b, ",\"logos\":");
    sb_q(&b, in->names.logos, 0);
    sb_raw(&b, ",\"logos_gaf\":");
    sb_q(&b, in->names.logos_gaf, 0);
    sb_raw(&b, ",\"objects3d\":");
    sb_q(&b, in->names.objects3d, 0);
    sb_raw(&b, ",\"scripts\":");
    sb_q(&b, in->names.scripts, 0);
    sb_raw(&b, ",\"anims\":");
    sb_q(&b, in->names.anims, 0);
    sb_raw(&b, ",\"features\":");
    sb_q(&b, in->names.features, 0);
    sb_raw(&b, "},\"mounts\":[");
    for (i = 0; i < tagpu_vfs_narc(L.vfs); i++) {
        sb_raw(&b, "%s{\"archive\":", i ? "," : "");
        sb_q(&b, tagpu_vfs_arc_name(L.vfs, i), 1);
        sb_raw(&b, ",\"refused\":");
        sb_q(&b, tagpu_vfs_arc_refused(L.vfs, i), 0);
        sb_raw(&b, "}");
    }
    sb_raw(&b, "],\"bank\":[");
    for (i = 0; i < L.nbank; i++) {
        sb_raw(&b, "%s{\"gaf\":", i ? "," : "");
        sb_q(&b, L.bank[i].path, 1);
        sb_raw(&b, ",\"from\":");
        sb_q(&b, L.bank[i].src >= -1 ? tagpu_vfs_source(L.vfs, L.bank[i].src) : NULL, 1);
        sb_raw(&b, ",\"refused\":");
        sb_q(&b, L.bank[i].why[0] ? L.bank[i].why : NULL, 0);
        sb_raw(&b, "}");
    }
    sb_raw(&b, "],\"logos\":");
    if (L.has_logos) {
        sb_raw(&b, "{\"gaf\":");
        sb_q(&b, L.logos.path, 1);
        sb_raw(&b, ",\"from\":");
        sb_q(&b, tagpu_vfs_source(L.vfs, L.logos.src), 1);
        sb_raw(&b, ",\"refused\":");
        sb_q(&b, L.logos.why[0] ? L.logos.why : NULL, 0);
        sb_raw(&b, "}");
    } else sb_raw(&b, "null");
    sb_raw(&b, ",\"stages\":[");
    for (i = 0; i < nstage; i++) {
        if (i) sb_raw(&b, ",");
        stage_report(&b, &L, (const WIKI3DSTAGE*)AT(&L.a, st_at) + i, &srec[i]);
    }
    sb_raw(&b, "],\"units\":[");
    for (i = 0; i < nunit; i++) {
        if (i) sb_raw(&b, ",");
        unit_report(&b, &L, (const WIKI3DUNIT*)AT(&L.a, un_at) + i, &urec[i]);
    }
    sb_raw(&b, "],\"arena_bytes\":%lu}\n", (unsigned long)assets);
    /* the report has the reserve to itself */
    L.a.cap = cap;
    L.a.full = 0;
    w = NULL;
    if (!b.oom && b.s && (rep_at = put_str(&L.a, b.s, b.n)) != 0) {
        w = (WIKI3D*)AT(&L.a, w_at);
        w->nstage = nstage;
        w->stage = nstage ? (const WIKI3DSTAGE*)AT(&L.a, st_at) : NULL;
        w->nunit = nunit;
        w->unit = nunit ? (const WIKI3DUNIT*)AT(&L.a, un_at) : NULL;
        w->report = (const char*)AT(&L.a, rep_at);
        w->bytes = L.a.used;
    }
    for (i = 0; srec && i < nstage; i++) stage_free(&srec[i]);
    free(srec);
    free(urec);
    free(names);
    free(b.s);
    free(L.tex);
    free(L.bind);
    free(L.bank);
    tagpu_vfs_close(L.vfs);
    return w;
}

/* ---- the DLL's thread, publish and trigger --------------------------------------------------------- */

#ifdef _WIN32

#define ARENA_CAP   (32u << 20)     /* reserved whole; committed as the load grows into it       */
#define COMMIT_STEP (1u << 20)
#define DUMP_ASK    "tagpu_wiki3d.dump"
#define DUMP_OUT    "tagpu_wiki3d.json"

static volatile LONG s_started;
static PVOID volatile s_pub;            /* the WIKI3D, once: one interlocked store               */

static uint32_t commit_pages(unsigned char* base, uint32_t upto)
{
    uint32_t to = (upto + COMMIT_STEP - 1) & ~(COMMIT_STEP - 1);
    if (to > ARENA_CAP) to = ARENA_CAP;
    /* committing a page already committed leaves it as it is, so the prefix is asked again whole */
    return VirtualAlloc(base, to, MEM_COMMIT, PAGE_READWRITE) ? to : 0;
}

static DWORD WINAPI loader(LPVOID p)
{
    WIKI3DIN* in = (WIKI3DIN*)p;
    unsigned char* base;
    const WIKI3D* w;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    /* THE ARENA'S RANGE, checked once here: the block is refused whole unless both its ends pass the
       readers' filter, so every pointer in it does. It is never freed once published; one refused
       here was never seen by anyone. */
    if (!(base = (unsigned char*)VirtualAlloc(NULL, ARENA_CAP, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE))) {
        LOG("wiki3d: NOT loaded - %lu MB of address space could not be reserved (%lu)", (unsigned long)(ARENA_CAP >> 20),
            (unsigned long)GetLastError());
        free(in);
        return 0;
    }
    if (!tagpu_m3_ptr_ok(base) || !tagpu_m3_ptr_ok(base + ARENA_CAP - 1)) {
        LOG("wiki3d: NOT loaded - the arena at %p lies outside the readers' range", (void*)base);
        VirtualFree(base, 0, MEM_RELEASE);
        free(in);
        return 0;
    }
    w = tagpu_wiki3d_load(in, base, (uint32_t)(uintptr_t)base, ARENA_CAP, commit_pages);
    QueryPerformanceCounter(&t1);
    if (!w) {
        LOG("wiki3d: NOT loaded - the arena could not hold the report");
        free(in);
        return 0;
    }
    {
        int i, so = 0, uo = 0;
        for (i = 0; i < w->nstage; i++) so += w->stage[i].ok;
        for (i = 0; i < w->nunit; i++) uo += w->unit[i].ok;
        LOG("wiki3d: loaded in %.1f ms - arena %lu bytes at %p; %d of %d stages' ground and %d of %d unit types resolved",
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, (unsigned long)w->bytes, (void*)base,
            so, w->nstage, uo, w->nunit);
    }
    InterlockedExchangePointer((PVOID*)&s_pub, (PVOID)w);
    free(in);
    return 0;
}

void tagpu_wikiasset_start(WIKI3DIN* in)
{
    HANDLE t;
    if (InterlockedCompareExchange(&s_started, 1, 0) != 0) { free(in); return; }
    if (!(t = CreateThread(NULL, 0, loader, in, CREATE_SUSPENDED, NULL))) {
        LOG("wiki3d: NOT loaded - CreateThread failed (%lu)", (unsigned long)GetLastError());
        free(in);
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(t);
    CloseHandle(t);
}

const WIKI3D* tagpu_wiki3d_get(void)
{
    return (const WIKI3D*)InterlockedCompareExchangePointer((PVOID*)&s_pub, NULL, NULL);
}

void tagpu_wikiasset_poll(void)
{
    const WIKI3D* w;
    FILE* f;
    if (GetFileAttributesA(DUMP_ASK) == INVALID_FILE_ATTRIBUTES || !(w = tagpu_wiki3d_get())) return;
    DeleteFileA(DUMP_ASK);
    if (!(f = fopen(DUMP_OUT ".tmp", "wb"))) { LOG("wiki3d: the report could not open %s", DUMP_OUT ".tmp"); return; }
    fputs(w->report, f);
    if (fclose(f) != 0 || !MoveFileExA(DUMP_OUT ".tmp", DUMP_OUT, MOVEFILE_REPLACE_EXISTING)) {
        LOG("wiki3d: the report could not be written (%lu)", (unsigned long)GetLastError());
        return;
    }
    LOG("wiki3d: the report written to %s", DUMP_OUT);
}

#endif
