/* tagpu_wikiasset.c -- the in-game wiki's 3D assets and the mod's left sidebar from the player's
   archives. Contract, rules and invariants: tagpu_wikiasset.h.

   THREADS. tagpu_wikiasset_start runs on the game thread and only hands its input over;
   tagpu_wikiside_load and tagpu_wiki3d_load run on the loader thread it starts (or the native
   test's main thread) and touch nothing but their input, their own files, the sidebar's block and
   the arena; tagpu_wikiside_get, tagpu_wiki3d_get and the trigger read the published blocks, which
   nothing writes after their stores. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hpi.h"
#include "tagpu_json.h"
#include "tagpu_pak.h"
#include "tagpu_tdf.h"
#include "tagpu_guifile.h"
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
        /* the texture state past the name: the file's words there are data, and the material layer
           follows +0x18 as a pointer, so they start at 0 and only bind_all writes them */
        memset(d + PR_TEX + 4, 0, PR_FLAGS - (PR_TEX + 4));
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

/* ---- the game's cursors ----------------------------------------------------------------------------
   THE PLAYER'S OWN CURSORS for the stage: <anims>\cursors.GAF by the path rule the engine loads it by
   (0x429700("cursors") from 0x429C9A: 0x4290F0 builds <anims>\<name>.GAF with the language variant
   first, then 0x4B8C60 loads it), and from it the sequences the shapes show (tagpu_wikisim.h,
   WIKI_CURSOR_*) found as 0x4B8D40 finds a name, each copied once in the engine's form. A sequence
   that is absent or refused leaves its shapes on the kit's icons. */
static const char* const CURSOR_SEQ[3] = { "cursornormal", "cursormove", "cursornormal" };

typedef struct {
    char        gaf[TAGPU_VFS_PATHMAX], from[64], why[200];
    int         frames[3];              /* -1: not found or refused                              */
    const void* seq[3];
} CURREC;

static void cursors_load(LOADER* L, CURREC* r)
{
    TAGPU_VFILE f;
    unsigned char* blob;
    GAF g;
    int k;
    for (k = 0; k < 3; k++) r->frames[k] = -1;
    if (!tagpu_vfs_find_built(L->vfs, L->in->names.anims, "cursors", "GAF", &f)) {
        snprintf(r->why, sizeof r->why, "no source holds %s\\cursors.gaf", L->in->names.anims);
        return;
    }
    memcpy(r->gaf, f.path, sizeof r->gaf);
    strncpy(r->from, tagpu_vfs_source(L->vfs, f.src), sizeof r->from - 1);
    if (!(blob = load(L, &f, CAP_GAF, r->why, sizeof r->why)) || !gaf_open(&g, blob, f.size, r->why, sizeof r->why)) {
        free(blob);
        return;
    }
    for (k = 0; k < 3; k++) {
        int j, s, n = 0;
        uint32_t at;
        for (j = 0; j < k && strcmp(CURSOR_SEQ[j], CURSOR_SEQ[k]); j++) {}
        if (j < k) { r->seq[k] = r->seq[j]; r->frames[k] = r->frames[j]; continue; }
        if ((s = gaf_find(&g, CURSOR_SEQ[k])) < 0) {
            if (!r->why[0]) snprintf(r->why, sizeof r->why, "it has no sequence %s", CURSOR_SEQ[k]);
            continue;
        }
        if (!(at = gaf_seq(&L->a, &g, s, &n, r->why, sizeof r->why))) continue;
        r->seq[k] = AT(&L->a, at);
        r->frames[k] = n;
    }
    free(blob);
}

static void cursors_report(SB* b, const CURREC* r)
{
    int k, ok = 1;
    sb_raw(b, ",\"cursors\":{\"gaf\":");
    sb_q(b, r->gaf[0] ? r->gaf : NULL, 1);
    sb_raw(b, ",\"from\":");
    sb_q(b, r->gaf[0] ? r->from : NULL, 1);
    sb_raw(b, ",\"sequences\":[");
    for (k = 0; k < 3; k++) {
        sb_raw(b, "%s{\"name\":\"%s\",\"frames\":", k ? "," : "", CURSOR_SEQ[k]);
        if (r->frames[k] >= 0) sb_raw(b, "%d}", r->frames[k]); else sb_raw(b, "null}");
        if (!r->seq[k]) ok = 0;
    }
    sb_raw(b, "],\"ok\":%s,\"why\":", ok ? "true" : "false");
    sb_q(b, ok ? NULL : r->why, 0);
    sb_raw(b, "}");
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

/* an arena node's link field as an arena offset, 0 for none */
static uint32_t node_link(const ARENA* a, uint32_t node, uint32_t at) { return OFF(a, rd32(AT(a, node + at))); }

/* 0x4CB5F0 (DISASSEMBLED, 0x4CB5F0..0x4CB641): over the sibling chain from `node`, the most of 0
   and, for each node, its vertices' y and its child chain's height, each plus the node's y
   offset, in 16.16 with the engine's wrapping adds. `*left` counts the nodes it may still visit:
   the loader refused a model that reaches a node twice, so the tree holds at most MAXPIECE, and
   the count bounds the recursion as well. */
static int32_t model_top(const ARENA* a, uint32_t node, int* left)
{
    int32_t m = 0;
    for (; node && *left > 0; node = node_link(a, node, O_SIB)) {
        const uint32_t oy = rd32(AT(a, node + O_X + 4)), nv = rd32(AT(a, node + O_NVERT));
        const uint32_t vs = node_link(a, node, O_VERTS), ch = node_link(a, node, O_CHILD);
        uint32_t i;
        (*left)--;
        for (i = 0; vs && i < nv; i++) {
            const int32_t y = (int32_t)(rd32(AT(a, vs + 12 * i + 4)) + oy);
            if (y > m) m = y;
        }
        if (ch) {
            const int32_t y = (int32_t)((uint32_t)model_top(a, ch, left) + oy);
            if (y > m) m = y;
        }
    }
    return m;
}

/* THE MODEL'S SHAPE (WIKI3DUNIT) of the `nprim` posed primitives `prim`: each one's offset from
   its parent as the arena holds it, and its parent, found as the primitive whose child chain
   names it; tools/wiki/gfx.py `model_shape` is the same for the mockup's packs */
static void unit_shape(const ARENA* a, uint32_t root, const char* const* prim, int nprim, WIKI3DUNIT* u)
{
    int j, k, n, left = MAXPIECE;
    for (k = 0; k < nprim; k++) {
        const unsigned char* nd = (const unsigned char*)prim[k];
        u->offset[k][0] = (int32_t)rd32(nd + O_X);
        u->offset[k][1] = (int32_t)rd32(nd + O_X + 4);
        u->offset[k][2] = (int32_t)rd32(nd + O_Z);
        u->parent[k] = -1;
    }
    for (j = 0; j < nprim; j++) {
        uint32_t ch = node_link(a, (uint32_t)((const unsigned char*)prim[j] - a->base), O_CHILD);
        for (n = 0; ch && n < nprim; n++, ch = node_link(a, ch, O_SIB))
            for (k = 0; k < nprim; k++)
                if ((const unsigned char*)prim[k] == AT(a, ch)) u->parent[k] = (int16_t)j;
    }
    u->top = model_top(a, root, &left);
}

/* THE SCRIPT FOR THE WIKI'S COB HOST: the bytes copied into the arena, and the posed model's
   order and visibility (WIKI3DUNIT). An arena too full to take the bytes leaves the type without
   a script, as its log line says; a full arena stays full (alloc), so every later allocation of
   the load fails with it. */
static void unit_script(LOADER* L, const unsigned char* cob, uint32_t n, uint32_t root, WIKI3DUNIT* u)
{
    const char* walk[MAXPIECE];
    const char* prim[MAXPIECE];
    const char* pname[MAXPIECE];
    int np = 0, nprim, k;
    const uint32_t at = alloc(&L->a, n);
    if (!at) { LOG("wiki3d: the arena has no room for %s's script", u->name); return; }
    memcpy(AT(&L->a, at), cob, n);
    u->script = AT(&L->a, at);
    u->nscript = n;
    nprim = piece_orders(&L->a, root, walk, &np, prim, pname);
    if (nprim < 0 || tagpu_cobscan_posed((const char*)cob, n, 0, prim, pname, nprim) < 0) return;
    for (k = 0; k < nprim; k++) u->visible[k] = rd32((const unsigned char*)prim[k] + O_NVERT) >= 3;
    unit_shape(&L->a, root, prim, nprim, u);
    for (k = 0; k < np; k++) {
        int j;
        u->posed_of[k] = -1;
        for (j = 0; j < nprim; j++)
            if (prim[j] == walk[k]) { u->posed_of[k] = (int16_t)j; break; }
    }
    u->nwalk = np;
    u->pieces = nprim;
}

/* the report's "posed": null without a script; else the posed order's names and each primitive's
   visibility as a string of 0 and 1, both empty when the swap refused */
static void unit_posed_report(SB* b, const LOADER* L, const WIKI3DUNIT* u, const UNITREC* r)
{
    const char* walk[MAXPIECE];
    const char* prim[MAXPIECE];
    const char* pname[MAXPIECE];
    int np = 0, k;
    if (!u->script) { sb_raw(b, "null"); return; }
    sb_raw(b, "{\"pieces\":[");
    if (u->pieces && piece_orders(&L->a, r->md.root, walk, &np, prim, pname) == u->pieces &&
        tagpu_cobscan_posed((const char*)u->script, u->nscript, 0, prim, pname, u->pieces) == u->pieces)
        for (k = 0; k < u->pieces; k++) {
            const uint32_t nm = OFF(&L->a, rd32((const unsigned char*)prim[k] + O_NAME));
            sb_raw(b, k ? "," : "");
            sb_q(b, nm ? (const char*)AT(&L->a, nm) : "", 0);
        }
    sb_raw(b, "],\"visible\":\"");
    for (k = 0; k < u->pieces; k++) sb_raw(b, u->visible[k] ? "1" : "0");
    sb_raw(b, "\"}");
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
            unit_script(L, cob, r->sf.size, r->md.root, u);
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
    sb_raw(b, ",\"posed\":");
    unit_posed_report(b, L, u, r);
    if (r->info)
        sb_raw(b, ",\"facts\":{\"mobile\":%s,\"slant\":%s,\"shadow\":%s}", u->mobile ? "true" : "false",
               u->slant ? "true" : "false", u->shadow ? "true" : "false");
    else
        sb_raw(b, ",\"facts\":null");
    sb_raw(b, ",\"ok\":%s,\"why\":", u->ok ? "true" : "false");
    sb_q(b, u->ok ? NULL : r->why, 0);
    sb_raw(b, "}");
}

/* ---- the left sidebar ----------------------------------------------------------------------------
   The resolution tagpu_wikiasset.h describes, made step for step as tools/wiki/gfx.py `side_pack`
   makes it, so tools/wiki/assetcheck.py can hold the two equal: the same files asked in the same
   order, every frame registered in the same order (which is the sheet's packing order too). Each
   step cites the engine code it follows; where gfx.py departs from the engine, so does this, and
   tagpu_wikiasset.h lists where. */

#define CAP_SIDE   (1u << 20)   /* a .GUI, TDF or FBI read whole: the stock files are under 64 KB */
#define CAP_PCX    (4u << 20)
#define SNAME      64           /* a name as a file gives it; the layout keeps WIKI_SIDE_NAME     */
#define FRAME_MAXW WIKISIDE_SHEET_W
#define FRAME_MAXH 4096
#define LAYERS_MAX 255          /* the leaves one frame stacks: its byte sub-frame count, one level */
#define LAYERS_PX  (16u << 20)  /* their pixels together: four of the largest leaf (FRAME_MAXW x
                                   FRAME_MAXH), so one frame never holds more than 16 MB of leaves */
#define GAF_SIG    0x00010100u
/* GUI_ButtonDraw 0x4A5F40 and the stage pass's id-1 case 0x4A8B4B read these attribs bits */
#define ATTR_CHECKBOX  0x80u
#define ATTR_LAST_GREY 0x100u
#define ATTR_SCROLL    0x1800u
#define ATTR_ONOFF     0x4000u
#define GREY_ROW       (32 - 0x14)  /* GUI_ButtonDraw's box-shader level -20 (push at 0x4A69B6) */

#define TITLE_RAW  0x1F         /* the display name's bytes: def+0x00 is 0x20 bytes             */
/* The names a page's buttons are read and pressed by (DISASSEMBLED): a .GUI gadget's name is read
   into the record's 16 bytes at +2 by 0x4C48C0(.., 0x10), which copies and ends the string at its
   last byte (0x4AD39C, 0x4C4949..0x4C4951), and a download entry's UNITNAME by 0x4C48C0(.., 0x20)
   (0x42DED8); the fill copies the entry's whole name into the record (strcpy, 0x41AE84..0x41AEAC);
   a press reads the record's name cut to 16 characters (0x49FED0: strncpy 0x10, [0x10] = 0). */
#define NAME_GUI   15
#define NAME_DL    31
#define NAME_PRESS 16

/* An FBI's UNITINFO as the cast rule and the casting read it (gfx.py `unit_record`): integers as
   the engine's atoi (tagpu_guifile_num: `num` saturated for comparisons, `bits` for flag tests),
   fractional figures as strtod over the first decimal number (fbi_strtod), a missing key 0. */
typedef struct {
    char      name[SNAME];              /* the UnitName as its FBI spells it                     */
    int       canstop, activate_when_built, src, order;
    char      path[TAGPU_VFS_PATHMAX];
    long long bmcode, canfly, fx, fz;
    uint32_t  builder;
    double    workertime, maxvelocity, mwd, extracts, energymake, energyuse, buildtime, builddistance;
    int       has_mwd;                  /* MinWaterDepth given: a unit without it is a land unit  */
    int       weapon1, fort;            /* Weapon1 not blank; TEDClass FORT without case          */
    int       has_g;                    /* the yard holds a G cell (yard_g)                       */
    uint16_t  g[WIKI_FP_MAX];           /* its G rows, when the footprint is within WIKI_FP_MAX   */
    unsigned char title[TITLE_RAW];     /* the display name's code-page bytes; ntitle 0: the name */
    int       ntitle;
} SUNIT;
typedef struct { char unitmenu[SNAME], unitname[SNAME]; uint32_t menu; long long button; int file; } SENTRY;
typedef struct { char name[SNAME], prefix[SNAME], intgaf[SNAME], commander[SNAME]; } SSIDE;

/* a GAF of <anims>, opened once per name asked and read by the page (bank_get); `ok` 0 when
   absent or refused */
typedef struct {
    char            ask[SNAME];
    TAGPU_VFILE     f;
    uint32_t        n, nseq, npage;
    unsigned char** page;               /* npage pages, NULL until read                         */
    int             ok, src;
    int             unread;             /* a page read failed, and was reported                   */
    char            path[TAGPU_VFS_PATHMAX];
} SBANK;

/* a frame the sheet holds, registered once under its key */
typedef struct {
    char*          key;
    int            w, h, hx, hy, tkey;  /* tkey: its transparent index, -1 none                  */
    unsigned char* px;
    int            x, y;                /* its place in the sheet                                */
} SFRAME;

typedef struct { int n; int* f; } SSEQ; /* a sequence's frames, as registry indices             */

typedef struct {
    int       from, nf, f[3], src, key;
    long long attribs;
    char      seq[SNAME], text[WIKISIDE_TEXT], path[TAGPU_VFS_PATHMAX];
} SPIC;

/* one .GUI section as the gadget reader 0x4AD350/0x4ADC70 fills its record */
typedef struct {
    int       index, ok, filled;
    long long id, x, y, w, h, active, stages, attribs;
    uint32_t  attr, common, gaffile, grey;      /* the low bits, for flag tests               */
    char      name[SNAME], orig[SNAME], text[WIKISIDE_TEXT], panel[SNAME];
} SREC;

typedef struct {
    int           live;
    char          name[WIKI_SIDE_NAME], file[SNAME], path[TAGPU_VFS_PATHMAX];
    int           src, y, w, h, ng;
    WIKIGADGET    g[WIKI_SIDE_GADGETS];
    SPIC          pic[WIKI_SIDE_GADGETS];
    int           nlabel;
    WIKISIDELABEL label[WIKISIDE_LABELS];
    int           panel, phx, phy, pkey, psrc;  /* panel: a registry index, -1 none          */
    char          pfrom[SNAME + 32], ppath[TAGPU_VFS_PATHMAX];
} SSCREEN;

typedef struct {
    const WIKI3DIN* in;
    TAGPU_VFS*      vfs;
    SB              rep;
    int             nrep;
    SUNIT*          unit;  int nunit, cunit;
    SSIDE           side[WIKI_SIDE_SIDES];
    int             nside;
    SENTRY*         dl;    int ndl, cdl;
    char          (*dlfile)[TAGPU_VFS_PATHMAX]; int ndlfile, cdlfile;
    SBANK*          bank;  int nbank, cbank;
    int             common;             /* commongui's bank, -1 when it did not load            */
    SFRAME*         fr;    int nfr, cfr;
    SSCREEN*        scr;   int nscr, cscr;
    int             failed;             /* memory ran out: the sidebar is refused               */
    int             quiet;              /* the menus' reads report nothing (gfx.py `menu_of`)    */
} SIDE;

static int side_grow(SIDE* S, void** arr, int* cap, int need, size_t each)
{
    void* p;
    int c;
    if (need <= *cap) return 1;
    c = *cap ? *cap * 2 : 32;
    while (c < need) c *= 2;
    if (c > (1 << 24) || !(p = realloc(*arr, (size_t)c * each))) { S->failed = 1; return 0; }
    *arr = p;
    *cap = c;
    return 1;
}

static void lowcpy(char* d, const char* s, size_t n)
{
    size_t k;
    for (k = 0; k + 1 < n && s[k]; k++) d[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);
    d[k] = 0;
}

static void upcpy(char* d, const char* s, size_t n)
{
    size_t k;
    for (k = 0; k + 1 < n && s[k]; k++) d[k] = (char)(s[k] >= 'a' && s[k] <= 'z' ? s[k] - 32 : s[k]);
    d[k] = 0;
}

static void scopy(char* d, const char* s, size_t n)
{
    snprintf(d, n, "%s", s ? s : "");
}

static int ieq(const char* a, const char* b) { return !tagpu_hpi_stricmp(a, b, (size_t)-1); }

/* one entry of the report: {"element": ..., <kind>: ...} */
static void side_rep(SIDE* S, const char* kind, const char* fmt, const char* elem, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 5)))
#endif
    ;
static void side_rep(SIDE* S, const char* kind, const char* fmt, const char* elem, ...)
{
    char t[400];
    va_list ap;
    if (S->quiet) return;
    va_start(ap, elem);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    sb_raw(&S->rep, "%s{\"element\":", S->nrep++ ? "," : "");
    sb_q(&S->rep, elem, 0);
    sb_raw(&S->rep, ",\"%s\":", kind);
    sb_q(&S->rep, t, 0);
    sb_raw(&S->rep, "}");
}

/* Engine.load: 0x4290F0's path, the language's folder first, and a 0-byte file missing (the open
   test 0x4BBC40 returns the size, and every probe tests it) */
static int side_find(SIDE* S, const char* dir, const char* name, const char* ext, TAGPU_VFILE* f)
{
    return tagpu_vfs_find_built(S->vfs, dir, name, ext, f) && f->size > 0;
}

static unsigned char* side_read(SIDE* S, const TAGPU_VFILE* f, uint32_t cap, const char* what)
{
    char why[200];
    unsigned char* b = tagpu_vfs_load(S->vfs, f, cap, why, sizeof why);
    if (!b) side_rep(S, "refused", "%s: %s", what, f->path, why);
    return b;
}

/* ---- the GAFs, as gfx.py's Gaf reads them ----
   READ BY THE PAGE, never whole: a page is BANK_PAGE bytes at a multiple of it, the archive's own
   chunk (TAGPU_HPI_CHUNK), read once when a lookup first touches it and kept. A lookup touches the
   sequence table, the headers and the frames it follows, so a large bank costs the pages its
   sidebar sequences lie in (Escalation's fx.GAF is 82 MB, MEASURED 2026-10-27, and its radlogo lies
   in its last page). Every offset is bounded against the file's size before any page is read. */
#define BANK_PAGE  0x10000u
#define BANK_MAX   (1u << 30)

/* `len` bytes at `off` of the bank into `dst`; 0 when they are not all inside the file or a page
   could not be read */
static int bank_get(SIDE* S, SBANK* k, uint32_t off, uint32_t len, void* dst)
{
    unsigned char* d = (unsigned char*)dst;
    if (!inside(k->n, off, len)) return 0;
    while (len) {
        const uint32_t p = off / BANK_PAGE, o = off % BANK_PAGE, take = len < BANK_PAGE - o ? len : BANK_PAGE - o;
        if (!k->page[p]) {
            const uint32_t size = k->n - p * BANK_PAGE < BANK_PAGE ? k->n - p * BANK_PAGE : BANK_PAGE;
            char why[200];
            if (!(k->page[p] = (unsigned char*)malloc(size))) { S->failed = 1; return 0; }
            if (!tagpu_vfs_read(S->vfs, &k->f, p * BANK_PAGE, size, k->page[p], why, sizeof why)) {
                free(k->page[p]);
                k->page[p] = NULL;
                if (!k->unread) side_rep(S, "refused", "%s", k->path, why);
                k->unread = 1;
                return 0;
            }
        }
        memcpy(d, k->page[p] + o, take);
        d += take;
        off += take;
        len -= take;
    }
    return 1;
}

static int bank_u16(SIDE* S, SBANK* k, uint32_t off, uint32_t* v)
{
    unsigned char t[2];
    if (!bank_get(S, k, off, 2, t)) return 0;
    *v = rd16(t);
    return 1;
}

static int bank_u32(SIDE* S, SBANK* k, uint32_t off, uint32_t* v)
{
    unsigned char t[4];
    if (!bank_get(S, k, off, 4, t)) return 0;
    *v = rd32(t);
    return 1;
}

/* sig 0x00010100, a u32 sequence count, and each sequence's header and frame table in the file */
static int gaf_check(SIDE* S, SBANK* k)
{
    uint32_t i, count, sig;
    if (!bank_u32(S, k, 0, &sig) || sig != GAF_SIG || !bank_u32(S, k, 4, &count) || !inside(k->n, 12, (uint64_t)count * 4))
        return 0;
    for (i = 0; i < count; i++) {
        uint32_t so, nf;
        if (!bank_u32(S, k, 12 + 4 * i, &so) || !bank_u16(S, k, so, &nf) || !inside(k->n, so + SQ_TAB, (uint64_t)nf * 8))
            return 0;
    }
    k->nseq = count;
    return 1;
}

/* the first sequence named `name` without case (0x4B8D40), its header's offset, or 0 */
static uint32_t gaf_seq_at(SIDE* S, SBANK* k, const char* name)
{
    uint32_t i;
    for (i = 0; i < k->nseq; i++) {
        uint32_t so, room;
        char nm[33];
        if (!bank_u32(S, k, 12 + 4 * i, &so) || so > k->n - 8) continue;
        room = k->n - so - 8 < 32 ? k->n - so - 8 : 32;
        memset(nm, 0, sizeof nm);
        if (!bank_get(S, k, so + 8, room, nm)) continue;
        if (ieq(nm, name)) return so;
    }
    return 0;
}

static void bank_free(SBANK* k)
{
    uint32_t p;
    for (p = 0; k->page && p < k->npage; p++) free(k->page[p]);
    free(k->page);
}

static int side_bank(SIDE* S, const char* name)
{
    SBANK* k;
    int i;
    for (i = 0; i < S->nbank; i++)
        if (ieq(S->bank[i].ask, name)) return S->bank[i].ok ? i : -1;
    if (!side_grow(S, (void**)&S->bank, &S->cbank, S->nbank + 1, sizeof *S->bank)) return -1;
    k = &S->bank[S->nbank++];
    memset(k, 0, sizeof *k);
    scopy(k->ask, name, sizeof k->ask);
    if (!side_find(S, S->in->names.anims, name, "GAF", &k->f)) return -1;
    memcpy(k->path, k->f.path, sizeof k->path);
    k->src = k->f.src;
    k->n = k->f.size;
    /* below 1 GiB, an offset read out of the file plus a header's or a frame table's length
       (at most 8 x 65535 + 48 bytes) cannot wrap 32 bits */
    if (k->n > BANK_MAX) {
        side_rep(S, "refused", "%s: %lu bytes, past the reader's bound", "a GAF", k->path, (unsigned long)k->n);
        return -1;
    }
    k->npage = (k->n + BANK_PAGE - 1) / BANK_PAGE;
    if (!(k->page = (unsigned char**)calloc(k->npage, sizeof *k->page))) { S->failed = 1; return -1; }
    if (!gaf_check(S, k)) {
        if (!k->unread) side_rep(S, "refused", "%s: not a GAF this reader takes", "a GAF", k->path);
        return -1;
    }
    k->ok = 1;
    return S->nbank - 1;
}

/* gfx.py's ta3do.gaf_frame over a leaf: the frame's pixels decoded into w x h, its transparent
   index everywhere nothing is written; 0 for a frame this reader refuses */
static int leaf_decode(SIDE* S, SBANK* k, uint32_t o, int* w, int* h, int* tk, unsigned char** out)
{
    unsigned char hd[GF_SIZE], *px, *row = NULL;
    uint32_t W, H, ptr, pos, r;
    if (!bank_get(S, k, o, GF_SIZE, hd)) return 0;
    W = rd16(hd);
    H = rd16(hd + 2);
    ptr = rd32(hd + 16);
    /* +0x0B is the alpha-blit flag, not part of the sub-frame count (a byte at +0x0A, 0x4B8CC0):
       the leaf's pixels are copied like any other's, as ta3do.gaf_compose copies them */
    if (W > FRAME_MAXW || H > FRAME_MAXH) return 0;
    if (!(px = (unsigned char*)malloc(W && H ? W * H : 1))) return 0;
    memset(px, hd[8], W * H);
    if (!hd[9]) {
        if (ptr < k->n && !bank_get(S, k, ptr, k->n - ptr < W * H ? k->n - ptr : W * H, px)) { free(px); return 0; }
    } else if (H) {
        /* a row: its u16 byte count, then that many bytes; a literal run may read up to 63 bytes
           past the row's end, as the pack's reader does, so the row is fetched with that margin */
        if (!(row = (unsigned char*)malloc(0x10000u + 64u))) { free(px); return 0; }
        pos = ptr;
        for (r = 0; r < H; r++) {
            uint32_t end, avail, at, col = 0, cnt;
            if (!bank_u16(S, k, pos, &cnt)) { free(row); free(px); return 0; }
            pos += 2;
            end = pos + cnt;
            avail = (uint64_t)end + 64 < k->n ? end + 64 : k->n;
            if (pos < avail && !bank_get(S, k, pos, avail - pos, row)) { free(row); free(px); return 0; }
            at = pos;
            while (at < end && col < W) {
                unsigned mask;
                if (at >= k->n) { free(row); free(px); return 0; }
                mask = row[at - pos];
                at++;
                if (mask & 1) col += mask >> 1;
                else if (mask & 2) {
                    unsigned j, n = (mask >> 2) + 1;
                    unsigned char v;
                    if (at >= k->n) { free(row); free(px); return 0; }
                    v = row[at - pos];
                    at++;
                    for (j = 0; j < n; j++) if (col < W) px[r * W + col++] = v;
                } else {
                    unsigned j, n = (mask >> 2) + 1;
                    for (j = 0; j < n; j++)
                        if (col < W && at + j < k->n) px[r * W + col++] = row[at + j - pos];
                    at += n;
                }
            }
            pos = end;
        }
        free(row);
    }
    *w = (int)W;
    *h = (int)H;
    *tk = hd[8];
    *out = px;
    return 1;
}

typedef struct { int w, h, tk, lx, ly; unsigned char* px; } LAYER;
typedef struct { LAYER* l; int n, cap, bad; uint32_t px; const char* why; } LAYERS;

static void leaves(SIDE* S, SBANK* k, uint32_t o, int depth, LAYERS* L)
{
    unsigned char hd[20];
    unsigned sub, j;
    uint32_t ptr;
    if (L->bad) return;
    if (!bank_get(S, k, o, 20, hd)) { L->bad = 1; return; }
    sub = hd[GF_SUBN];
    ptr = rd32(hd + 16);
    if (!sub) {
        LAYER* y;
        /* THE BOUND ON ONE FRAME'S MEMORY: every leaf is decoded and held until the frame is
           composed, and one crafted frame can stack LAYERS_MAX leaves of FRAME_MAXW x FRAME_MAXH
           (1 GB). The running sum of their pixels is held to LAYERS_PX, checked on the leaf's
           header before its pixels are allocated; past it the frame is refused, with the reason.
           A leaf past FRAME_MAXW x FRAME_MAXH is leaf_decode's to refuse. gfx.py has no such
           bound: a file past it is one the two readers part on. */
        {
            const uint32_t lw = rd16(hd), lh = rd16(hd + 2);
            if (lw <= FRAME_MAXW && lh <= FRAME_MAXH && (uint64_t)L->px + (uint64_t)lw * lh > LAYERS_PX) {
                L->why = "its sub-frames hold more than 16 MB of pixels";
                L->bad = 1;
                return;
            }
            L->px += lw * lh;
        }
        if (L->n == L->cap) {
            LAYER* t;
            if (L->cap >= LAYERS_MAX || !(t = (LAYER*)realloc(L->l, (size_t)(L->cap ? L->cap * 2 : 4) * sizeof *t))) { L->bad = 1; return; }
            L->l = t;
            L->cap = L->cap ? L->cap * 2 : 4;
        }
        y = &L->l[L->n];
        if (!leaf_decode(S, k, o, &y->w, &y->h, &y->tk, &y->px)) { L->bad = 1; return; }
        y->lx = (int16_t)rd16(hd + 4);
        y->ly = (int16_t)rd16(hd + 6);
        L->n++;
        return;
    }
    /* A STACK IS ONE LEVEL DEEP: the GAF loader 0x4B8C60 rebases each sub-frame offset and that
       sub-frame's +0x10, never the entries of a sub-frame's own table (0x4B8CC0..0x4B8CF5,
       DISASSEMBLED), so the engine would read a nested table's file offsets as pointers. The
       pack's reader (ta3do.gaf_compose) refuses the same frame; this also holds a frame to 255
       leaves. */
    if (depth > 0) {
        L->why = "a sub-frame has sub-frames of its own, which the engine's GAF loader does not rebase";
        L->bad = 1;
        return;
    }
    if (!inside(k->n, ptr, (uint64_t)sub * 4)) { L->bad = 1; return; }
    for (j = 0; j < sub && !L->bad; j++) {
        uint32_t so;
        if (!bank_u32(S, k, ptr + 4 * j, &so)) { L->bad = 1; return; }
        leaves(S, k, so, depth + 1, L);
    }
}

/* Gaf.drawn: frame `fo` as CopyGafToContext 0x4B7F90 draws it. A frame with sub-frames draws none
   of its own pixels: the blit calls itself for each sub-frame in order with the SAME point
   (0x4B7FC8..0x4B801A), each landing at that point minus its own hotspot (0x4B804A..0x4B805A)
   and skipping its own transparent index. The layers are composed into the frame's own w x h box,
   which stands at the point minus the frame's hotspot; a layer pixel outside the box is left out
   (counted in `*outside`). The key is the frame's transparent index unless a layer draws that
   index, then the lowest index none draws. 0 for a frame this reader refuses, with `*why` the
   reason when it is the bound on the leaves' memory (LAYERS_PX) or a nested stack, else NULL. */
static int frame_drawn(SIDE* S, SBANK* k, uint32_t fo, int* w, int* h, int* hx, int* hy, int* tk,
                       unsigned char** out, long* outside, const char** why)
{
    LAYERS L;
    unsigned char hd[9], used[256], *px, *drawn;
    int W, H, HX, HY, key, i;
    memset(&L, 0, sizeof L);
    *outside = 0;
    if (!bank_get(S, k, fo, 9, hd)) return 0;
    W = rd16(hd);
    H = rd16(hd + 2);
    HX = (int16_t)rd16(hd + 4);
    HY = (int16_t)rd16(hd + 6);
    key = hd[8];
    leaves(S, k, fo, 0, &L);
    *why = L.why;
    if (L.bad || W > FRAME_MAXW || H > FRAME_MAXH) {
        for (i = 0; i < L.n; i++) free(L.l[i].px);
        free(L.l);
        return 0;
    }
    if (L.n == 1 && L.l[0].lx == HX && L.l[0].ly == HY && L.l[0].w == W && L.l[0].h == H) {
        *w = W; *h = H; *hx = HX; *hy = HY; *tk = L.l[0].tk; *out = L.l[0].px;
        free(L.l);
        return 1;
    }
    px = (unsigned char*)calloc((size_t)(W && H ? W * H : 1), 1);
    drawn = (unsigned char*)calloc((size_t)(W && H ? W * H : 1), 1);
    memset(used, 0, sizeof used);
    for (i = 0; px && drawn && i < L.n; i++) {
        const LAYER* y = &L.l[i];
        const int ox = HX - y->lx, oy = HY - y->ly;
        int r, c;
        for (r = 0; r < y->h; r++)
            for (c = 0; c < y->w; c++) {
                const unsigned char v = y->px[r * y->w + c];
                const int x = ox + c, yy = oy + r;
                if (v == y->tk) continue;
                if (x >= 0 && x < W && yy >= 0 && yy < H) { px[yy * W + x] = v; drawn[yy * W + x] = 1; used[v] = 1; }
                else (*outside)++;
            }
    }
    for (i = 0; i < L.n; i++) free(L.l[i].px);
    free(L.l);
    if (!px || !drawn) { free(px); free(drawn); return 0; }
    if (used[key]) {
        for (i = 0; i < 256 && used[i]; i++) {}
        if (i < 256) key = i;
    }
    for (i = 0; i < W * H; i++) if (!drawn[i]) px[i] = (unsigned char)key;
    free(drawn);
    *w = W; *h = H; *hx = HX; *hy = HY; *tk = key; *out = px;
    return 1;
}

/* the registry: a frame once per key, in the order first asked, which is the sheet's order */
static int frame_find(const SIDE* S, const char* key)
{
    int i;
    for (i = 0; i < S->nfr; i++) if (!strcmp(S->fr[i].key, key)) return i;
    return -1;
}

static int frame_add(SIDE* S, const char* key, int w, int h, int hx, int hy, int tk, unsigned char* px)
{
    SFRAME* f;
    const size_t kn = strlen(key);
    if (!side_grow(S, (void**)&S->fr, &S->cfr, S->nfr + 1, sizeof *S->fr)) { free(px); return -1; }
    f = &S->fr[S->nfr];
    if (!(f->key = (char*)malloc(kn + 1))) { S->failed = 1; free(px); return -1; }
    memcpy(f->key, key, kn + 1);
    f->w = w; f->h = h; f->hx = hx; f->hy = hy; f->tkey = tk; f->px = px;
    f->x = f->y = 0;
    return S->nfr++;
}

/* SideArt.seq: every frame of sequence `name` of bank `bk`, registered under
   <path>:<source>:<NAME>:<k>; 0 when the bank or the sequence is absent, or a frame is refused (a
   frame of no pixels too, which no picture can show) */
static int side_seq(SIDE* S, int bk, const char* name, SSEQ* q)
{
    SBANK* k;
    uint32_t so, nf, i;
    char key[TAGPU_VFS_PATHMAX + 2 * SNAME + 16], lp[TAGPU_VFS_PATHMAX], up[SNAME];
    q->n = 0;
    q->f = NULL;
    if (bk < 0 || !name || !name[0]) return 0;
    k = &S->bank[bk];
    if (!(so = gaf_seq_at(S, k, name)) || !bank_u16(S, k, so, &nf) || !nf) return 0;
    if (!(q->f = (int*)malloc(nf * sizeof *q->f))) { S->failed = 1; return 0; }
    lowcpy(lp, k->path, sizeof lp);
    upcpy(up, name, sizeof up);
    for (i = 0; i < nf; i++) {
        int w = 0, h = 0, hx, hy, tk, at;
        long outside;
        uint32_t fo;
        unsigned char* px = NULL;
        const char* why = NULL;
        snprintf(key, sizeof key, "%s:%s:%s:%lu", lp, tagpu_vfs_source(S->vfs, k->src), up, (unsigned long)i);
        if ((at = frame_find(S, key)) < 0) {
            if (!bank_u32(S, k, so + SQ_TAB + 8 * i, &fo) ||
                !frame_drawn(S, k, fo, &w, &h, &hx, &hy, &tk, &px, &outside, &why) || w < 1 || h < 1) {
                free(px);
                side_rep(S, "refused", "frame %lu of %s is one this reader does not take%s%s", k->path, (unsigned long)i, up,
                         why ? ": " : "", why ? why : "");
                free(q->f);
                q->f = NULL;
                return 0;
            }
            if (outside) side_rep(S, "note", "%ld sub-frame pixels fall outside the frame's box and are left out", key, outside);
            if ((at = frame_add(S, key, w, h, hx, hy, tk, px)) < 0) { free(q->f); q->f = NULL; return 0; }
        }
        q->f[q->n++] = at;
    }
    return 1;
}

/* ---- the files ---- */

/* Python's str.strip() over a Latin-1 string, which gfx.py applies to the UnitName, the commander,
   Weapon1 and TEDClass: blanks the TDF reader keeps (VT, FF, 0x1C..0x1F, 0x85, 0xA0) go too.
   `*n` is the length left after `*s` is moved past the leading ones. */
static int py_space(unsigned char c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0;
}

static void py_strip(const char** s, size_t* n)
{
    const char* p = *s ? *s : "";
    size_t k = strlen(p);
    while (k && py_space((unsigned char)*p)) { p++; k--; }
    while (k && py_space((unsigned char)p[k - 1])) k--;
    *s = p;
    *n = k;
}

/* gfx.py `strtod`: blanks (space, TAB, CR, LF), then the longest [+-]?(D+.?D*|.D+)([eE][+-]?D+)?,
   read as C's strtod reads that text alone; 0 when there is none or the key is missing. The match
   is copied before it is converted, so C's own extensions (hexadecimal, inf, nan, its wider blanks)
   never apply. */
static double fbi_strtod(const char* v)
{
    char buf[128], *t = buf;
    const char* p = v ? v : "";
    size_t a, k;
    double d;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    k = (*p == '+' || *p == '-') ? 1 : 0;
    a = k;
    while (p[k] >= '0' && p[k] <= '9') k++;
    if (k > a) {
        if (p[k] == '.') k++;
        while (p[k] >= '0' && p[k] <= '9') k++;
    } else {
        if (p[k] != '.' || !(p[k + 1] >= '0' && p[k + 1] <= '9')) return 0.0;
        k++;
        while (p[k] >= '0' && p[k] <= '9') k++;
    }
    if (p[k] == 'e' || p[k] == 'E') {
        size_t e = k + 1;
        if (p[e] == '+' || p[e] == '-') e++;
        if (p[e] >= '0' && p[e] <= '9') {
            while (p[e] >= '0' && p[e] <= '9') e++;
            k = e;
        }
    }
    if (k >= sizeof buf && !(t = (char*)malloc(k + 1))) return 0.0;
    memcpy(t, p, k);
    t[k] = 0;
    d = strtod(t, NULL);
    if (t != buf) free(t);
    return d;
}

/* THE YARD'S G CELLS as the unit-def loader parses YardMap into def+0x14E (0x42CF9D..0x42D06B: the
   byte map 0x42D198 into the jump table 0x42D16C, DISASSEMBLED), gfx.py `yard_cells`: each char
   of `.CGOYcfowy` fills one cell, any other is skipped; the string is read to 0x3FF chars (GetString
   0x4C48C0's buffer, 0x42CF3E..0x42CF59), and every cell past its codes takes the last code, or `o`
   with none (tagpu_patches.c `yard_parse`, which Impure installs at 0x42CF5E). G (0x8F) is the only
   code with bit 0x80. So cell i is code i of the string, else its last: `has_g` asks that of the
   fx x fz cells without making them (a footprint is data and may be huge), and the rows are made
   only within WIKI_FP_MAX. */
static void yard_g(SUNIT* u, const char* yard)
{
    static const char codes[] = ".CGOYcfowy";
    char cell[0x3FF];
    const char* y = yard ? yard : "";
    int n = 0, i;
    long long total;
    for (i = 0; i < 0x3FF && y[i]; i++)
        if (strchr(codes, y[i])) cell[n++] = y[i];
    total = u->fx > 0x3FF || u->fz > 0x3FF ? 0x3FF + 1 : u->fx * u->fz;
    for (i = 0; i < n && i < total; i++) if (cell[i] == 'G') u->has_g = 1;
    if (total > n && n && cell[n - 1] == 'G') u->has_g = 1;
    if (u->fx <= WIKI_FP_MAX && u->fz <= WIKI_FP_MAX)
        for (i = 0; i < total; i++)
            if ((i < n ? cell[i] : n ? cell[n - 1] : 'o') == 'G') u->g[i / u->fx] |= (uint16_t)(1u << (i % u->fx));
}

typedef struct { SIDE* S; } FBIWALK;

static int fbi_each(void* ctx, const TAGPU_VFILE* f)
{
    SIDE* S = ((FBIWALK*)ctx)->S;
    char why[200], key[sizeof S->in->names.language + 8];
    unsigned char* b;
    TAGPU_GUIFILE* g;
    int sec;
    const char *un, *t;
    size_t nn;
    if (!f->size || !(b = tagpu_vfs_load(S->vfs, f, CAP_SIDE, why, sizeof why))) return 1;
    g = tagpu_guifile_parse(b, f->size, why, sizeof why);
    free(b);
    if (!g) { side_rep(S, "refused", "the engine's parser refuses it: %s", f->path, why); return 1; }
    sec = tagpu_guifile_find(g, 0, "UNITINFO");
    un = tagpu_guifile_get(g, sec, "unitname");
    py_strip(&un, &nn);
    if (nn && side_grow(S, (void**)&S->unit, &S->cunit, S->nunit + 1, sizeof *S->unit)) {
        SUNIT* u = &S->unit[S->nunit];
        uint32_t bits;
        memset(u, 0, sizeof *u);
        snprintf(u->name, sizeof u->name, "%.*s", (int)nn, un);
        /* canstop is def+0x245 bit 3, 0 when the key is missing (0x42C90B..0x42C936) */
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "canstop"), NULL, &bits);
        u->canstop = (int)(bits & 1u);
        /* activatewhenbuilt is def+0x241 bit 18, the key's `and 1` (0x42C6B9..0x42C6DE) */
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "activatewhenbuilt"), NULL, &bits);
        u->activate_when_built = (int)(bits & 1u);
        /* BMcode: a missing key reads 0, a structure: esi, zeroed at 0x42C0F0, is pushed as the
           default at 0x42C401 with no write to it between (DISASSEMBLED) */
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "bmcode"), &u->bmcode, NULL);
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "builder"), NULL, &u->builder);
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "canfly"), &u->canfly, NULL);
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "footprintx"), &u->fx, NULL);
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "footprintz"), &u->fz, NULL);
        u->workertime = fbi_strtod(tagpu_guifile_get(g, sec, "workertime"));
        u->maxvelocity = fbi_strtod(tagpu_guifile_get(g, sec, "maxvelocity"));
        if ((t = tagpu_guifile_get(g, sec, "minwaterdepth")) != NULL) { u->has_mwd = 1; u->mwd = fbi_strtod(t); }
        u->extracts = fbi_strtod(tagpu_guifile_get(g, sec, "extractsmetal"));
        u->energymake = fbi_strtod(tagpu_guifile_get(g, sec, "energymake"));
        u->energyuse = fbi_strtod(tagpu_guifile_get(g, sec, "energyuse"));
        u->buildtime = fbi_strtod(tagpu_guifile_get(g, sec, "buildtime"));
        u->builddistance = fbi_strtod(tagpu_guifile_get(g, sec, "builddistance"));
        t = tagpu_guifile_get(g, sec, "weapon1");
        py_strip(&t, &nn);
        u->weapon1 = nn > 0;
        t = tagpu_guifile_get(g, sec, "tedclass");
        py_strip(&t, &nn);
        u->fort = nn == 4 && !tagpu_hpi_stricmp(t, "FORT", 4);
        /* the yard is parsed only for a def whose BMcode is 0 (0x42CF30..0x42CF38) */
        if (!u->bmcode && u->fx > 0 && u->fz > 0) yard_g(u, tagpu_guifile_get(g, sec, "yardmap"));
        /* the display name: `<language>name` when the key exists, else `name` (0x4C58A0 tries
           <0x51FDC0><key> first and the plain key second), its first 31 bytes (def+0x00 is 0x20
           bytes, 0x42BFAC..0x42BFBB); none given and an empty one are the UnitName */
        snprintf(key, sizeof key, "%sname", S->in->names.language);
        if (!(t = tagpu_guifile_get(g, sec, key))) t = tagpu_guifile_get(g, sec, "name");
        for (nn = 0; t && t[nn] && nn < TITLE_RAW; nn++) u->title[nn] = (unsigned char)t[nn];
        u->ntitle = (int)nn;
        u->src = f->src;
        u->order = S->nunit;
        memcpy(u->path, f->path, sizeof u->path);
        S->nunit++;
    }
    tagpu_guifile_free(g);
    return !S->failed;
}

static int unit_cmp(const void* a, const void* b)
{
    const SUNIT *x = (const SUNIT*)a, *y = (const SUNIT*)b;
    const int c = tagpu_hpi_stricmp(x->name, y->name, (size_t)-1);
    return c ? c : x->order - y->order;
}

/* Every FBI of the units folder by its UnitName without case, the first file listed winning a
   name: 0x488B10 compares def+0x20, the FBI's `unitname`, with _stricmp over the def array */
static void units_index(SIDE* S)
{
    FBIWALK w;
    int i, n = 0;
    w.S = S;
    tagpu_vfs_enum(S->vfs, S->in->names.units, "*.fbi", 0, fbi_each, &w);
    qsort(S->unit, (size_t)S->nunit, sizeof *S->unit, unit_cmp);
    for (i = 0; i < S->nunit; i++)
        if (!n || !ieq(S->unit[n - 1].name, S->unit[i].name)) S->unit[n++] = S->unit[i];
    S->nunit = n;
}

static const SUNIT* unit_get(const SIDE* S, const char* name)
{
    int lo = 0, hi = S->nunit - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2, c = tagpu_hpi_stricmp(name, S->unit[mid].name, (size_t)-1);
        if (!c) return &S->unit[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}

/* <gamedata>\sidedata.TDF's [SIDE0], [SIDE1], ... up to the first missing one (0x431A60), the first
   section of a name winning as the engine's lookup 0x4C3410 does; 0 with the reason reported */
static int sides_read(SIDE* S)
{
    TAGPU_VFILE f;
    unsigned char* b;
    TAGPU_GUIFILE* g;
    char why[200], nm[16];
    int sec;
    if (!side_find(S, S->in->names.gamedata, "sidedata", "TDF", &f)) {
        side_rep(S, "refused", "no %s\\sidedata.tdf", "sidedata", S->in->names.gamedata);
        return 0;
    }
    if (!(b = side_read(S, &f, CAP_SIDE, "sidedata"))) return 0;
    g = tagpu_guifile_parse(b, f.size, why, sizeof why);
    free(b);
    if (!g) { side_rep(S, "refused", "%s: the engine's parser refuses it: %s", "sidedata", f.path, why); return 0; }
    for (;;) {
        snprintf(nm, sizeof nm, "SIDE%d", S->nside);
        if ((sec = tagpu_guifile_find(g, 0, nm)) < 0) break;
        if (S->nside == WIKI_SIDE_SIDES) {
            side_rep(S, "refused", "sides past the layout's %d are not read", "sidedata", WIKI_SIDE_SIDES);
            break;
        }
        scopy(S->side[S->nside].name, tagpu_guifile_get(g, sec, "name"), SNAME);
        scopy(S->side[S->nside].prefix, tagpu_guifile_get(g, sec, "nameprefix"), SNAME);
        scopy(S->side[S->nside].intgaf, tagpu_guifile_get(g, sec, "intgaf"), SNAME);
        {
            const char* c = tagpu_guifile_get(g, sec, "commander");
            size_t cn;
            py_strip(&c, &cn);
            snprintf(S->side[S->nside].commander, SNAME, "%.*s", (int)cn, c);
        }
        S->nside++;
    }
    tagpu_guifile_free(g);
    if (!S->nside) { side_rep(S, "refused", "%s has no [SIDE0]", "sidedata", f.path); return 0; }
    return 1;
}

static int dl_each(void* ctx, const TAGPU_VFILE* f)
{
    SIDE* S = (SIDE*)ctx;
    char why[200];
    unsigned char* b;
    TAGPU_GUIFILE* g;
    int i, file;
    /* the engine's load fails on size 0 (0x4C2F9A), and the file adds nothing */
    if (!f->size || !(b = tagpu_vfs_load(S->vfs, f, CAP_SIDE, why, sizeof why))) return 1;
    g = tagpu_guifile_parse(b, f->size, why, sizeof why);
    free(b);
    if (!g) { side_rep(S, "refused", "the engine's parser refuses it: %s", f->path, why); return 1; }
    if (!side_grow(S, (void**)&S->dlfile, &S->cdlfile, S->ndlfile + 1, sizeof *S->dlfile)) { tagpu_guifile_free(g); return 0; }
    file = S->ndlfile++;
    memcpy(S->dlfile[file], f->path, TAGPU_VFS_PATHMAX);
    for (i = 0; i < tagpu_guifile_count(g, 0); i++) {
        const int sec = tagpu_guifile_child(g, 0, i);
        const char* um = tagpu_guifile_get(g, sec, "unitmenu");
        SENTRY* e;
        long long v;
        if (!um) continue;              /* a section without UNITMENU is skipped (0x42DE20) */
        if (!side_grow(S, (void**)&S->dl, &S->cdl, S->ndl + 1, sizeof *S->dl)) break;
        e = &S->dl[S->ndl++];
        scopy(e->unitmenu, um, sizeof e->unitmenu);
        scopy(e->unitname, tagpu_guifile_get(g, sec, "unitname"), NAME_DL + 1);
        /* MENU and BUTTON through 0x4C46C0, 0 when missing; both are bytes in the engine's entry
           (MENU read at 0x41AE48, BUTTON stored at 0x42DEE7 and read at 0x41AE57), cut where used */
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "menu"), NULL, &e->menu);
        tagpu_guifile_num(tagpu_guifile_get(g, sec, "button"), &v, NULL);
        e->button = v;
        e->file = file;
    }
    tagpu_guifile_free(g);
    return !S->failed;
}

static void num_field(SIDE* S, const char* v, const char* key, const char* where, int index, long long* num, uint32_t* bits)
{
    if (!tagpu_guifile_num(v, num, bits) && v) {
        char el[SNAME + 16];
        snprintf(el, sizeof el, "%s#%d", where, index);
        side_rep(S, "note", "%s is not an integer: read as the engine's atoi reads it", el, key);
    }
}

/* one section's gadget record; `ok` 0 for a negative size, which the pack refuses */
static void gadget_record(SIDE* S, const TAGPU_GUIFILE* g, int sec, int index, const char* where, SREC* r)
{
    const int com = tagpu_guifile_find(g, sec, "COMMON");
    uint32_t b;
    memset(r, 0, sizeof *r);
    r->index = index;
    num_field(S, tagpu_guifile_get(g, com, "id"), "id", where, index, &r->id, NULL);
    num_field(S, tagpu_guifile_get(g, com, "xpos"), "xpos", where, index, &r->x, NULL);
    num_field(S, tagpu_guifile_get(g, com, "ypos"), "ypos", where, index, &r->y, NULL);
    num_field(S, tagpu_guifile_get(g, com, "width"), "width", where, index, &r->w, NULL);
    num_field(S, tagpu_guifile_get(g, com, "height"), "height", where, index, &r->h, NULL);
    num_field(S, tagpu_guifile_get(g, com, "attribs"), "attribs", where, index, &r->attribs, &r->attr);
    num_field(S, tagpu_guifile_get(g, com, "commonattribs"), "commonattribs", where, index, NULL, &r->common);
    num_field(S, tagpu_guifile_get(g, com, "active"), "active", where, index, &r->active, NULL);
    num_field(S, tagpu_guifile_get(g, com, "gaffile"), "gaffile", where, index, NULL, &b);
    r->gaffile = b & 1u;
    num_field(S, tagpu_guifile_get(g, sec, "grayedout"), "grayedout", where, index, NULL, &b);
    r->grey = b & 1u;
    num_field(S, tagpu_guifile_get(g, sec, "stages"), "stages", where, index, &r->stages, NULL);
    scopy(r->name, tagpu_guifile_get(g, com, "name"), NAME_GUI + 1);
    scopy(r->text, tagpu_guifile_get(g, sec, "text"), sizeof r->text);
    scopy(r->panel, tagpu_guifile_get(g, sec, "panel"), sizeof r->panel);
    if (r->w < 0 || r->h < 0) {
        char el[SNAME + 16];
        snprintf(el, sizeof el, "%s#%d", where, index);
        side_rep(S, "refused", "a negative size %lld x %lld", el, r->w, r->h);
        return;
    }
    r->ok = 1;
}

/* <gui>\<file>.GUI by position: its records, one a top-level section in file order, or NULL for a
   file the engine would not load (missing, 0 bytes), would exit on, or whose first gadget is no
   panel (id 0) */
static SREC* screen_read(SIDE* S, const char* file, int* n, TAGPU_VFILE* f)
{
    unsigned char* b;
    TAGPU_GUIFILE* g;
    SREC* r;
    char why[200];
    int i;
    if (!side_find(S, S->in->names.gui, file, "GUI", f)) {
        side_rep(S, "refused", "no %s\\%s.GUI (or 0 bytes)", file, S->in->names.gui, file);
        return NULL;
    }
    if (!(b = side_read(S, f, CAP_SIDE, file))) return NULL;
    g = tagpu_guifile_parse(b, f->size, why, sizeof why);
    free(b);
    if (!g) { side_rep(S, "refused", "%s: the engine's parser refuses it: %s", file, f->path, why); return NULL; }
    *n = tagpu_guifile_count(g, 0);
    if (!*n || !(r = (SREC*)calloc((size_t)*n, sizeof *r))) {
        if (*n) S->failed = 1;
        else side_rep(S, "refused", "%s: its first gadget is no panel (id 0)", file, f->path);
        tagpu_guifile_free(g);
        return NULL;
    }
    for (i = 0; i < *n; i++) gadget_record(S, g, tagpu_guifile_child(g, 0, i), i, file, &r[i]);
    tagpu_guifile_free(g);
    if (!r[0].ok || r[0].id != 0) {
        side_rep(S, "refused", "%s: its first gadget is no panel (id 0)", file, f->path);
        free(r);
        return NULL;
    }
    return r;
}

/* ---- the picture rule ---- */

/* the frames GUI_ButtonDraw 0x4A5F40 shows at rest, pressed and greyed (WIKISIDEPIC) */
static int button_frames(int n, int base, uint32_t attr, int out[3])
{
    int last;
    if (n <= 1) { out[0] = 0; return 1; }
    last = n - 1;
    out[0] = base < last ? base : last;
    out[1] = base + 1 < last ? base + 1 : last;
    out[2] = attr & ATTR_LAST_GREY ? last : ((2 < last ? 2 : last) + base < last ? (2 < last ? 2 : last) + base : last);
    return 3;
}

static void pic_frames(SPIC* p, const SSEQ* q, int base, uint32_t attr)
{
    int idx[3], k;
    p->nf = button_frames(q->n, base, attr, idx);
    for (k = 0; k < p->nf; k++) p->f[k] = q->f[idx[k]];
}

typedef struct { int how, bank, base; SSEQ q; char seq[SNAME]; } SENG;

/* The stage pass's own resolution of a button (0x4A8B4B): its name in the screen's bank, then in
   commongui (base 0); else commongui's generic picture (0x4A8C00..0x4A8DC9: CHECKBOX for attribs
   0x80; stagebuttn1 for Off|On, one stage or attribs 0x4000; stagebuttn<min(stages, 4)> for other
   stages; BUTTONS0), whose base is the frame (0, 4, 8, ...) nearest the gadget's size in |dw| +
   |dh|. 0 when none resolves. */
static int engine_picture(SIDE* S, const SREC* g, const char* name, const char* bank, SENG* e)
{
    char seq[SNAME];
    long long best = -1;
    int k;
    const int banks[2] = { side_bank(S, bank), S->common };
    memset(e, 0, sizeof *e);
    for (k = 0; k < 2; k++)
        if (name[0] && side_seq(S, banks[k], name, &e->q)) {
            e->how = k ? WIKISIDE_FROM_COMMONGUI : WIKISIDE_FROM_PAGE;
            e->bank = banks[k];
            upcpy(e->seq, name, sizeof e->seq);
            return 1;
        }
    if (S->common < 0) return 0;
    if (g->attr & ATTR_CHECKBOX) scopy(seq, "CHECKBOX", sizeof seq);
    else if (!strcmp(g->text, "Off|On") || g->stages == 1 || g->attr & ATTR_ONOFF) scopy(seq, "stagebuttn1", sizeof seq);
    else if (g->stages) snprintf(seq, sizeof seq, "stagebuttn%lld", g->stages < 4 ? g->stages : 4);
    else scopy(seq, "BUTTONS0", sizeof seq);
    if (!side_seq(S, S->common, seq, &e->q)) return 0;
    for (k = 0; k < e->q.n; k += 4) {
        const SFRAME* f = &S->fr[e->q.f[k]];
        const long long dw = g->w - f->w, dh = g->h - f->h;
        const long long d = (dw < 0 ? -dw : dw) + (dh < 0 ? -dh : dh);
        if (best < 0 || d < best) { best = d; e->base = k; }
    }
    e->how = WIKISIDE_FROM_FALLBACK;
    e->bank = S->common;
    upcpy(e->seq, seq, sizeof e->seq);
    return 1;
}

static void pic_from_bank(SIDE* S, SPIC* p, int how, int bank, const char* seq)
{
    p->from = how;
    p->src = S->bank[bank].src;
    memcpy(p->path, S->bank[bank].path, sizeof p->path);
    upcpy(p->seq, seq, sizeof p->seq);
}

/* gfx.py's 8-bit RLE PCX: (w, h, pixels as palette indices, the file's own palette not read); 0
   for any other kind */
static int pcx_read(const unsigned char* b, uint32_t n, int* w, int* h, unsigned char** out)
{
    uint32_t pitch, W, H, need, got = 0, pos = 128, r;
    unsigned char *line, *px;
    long x0, y0, x1, y1;
    if (n < 128 || b[0] != 10 || b[2] != 1 || b[3] != 8 || b[65] != 1) return 0;
    x0 = rd16(b + 4); y0 = rd16(b + 6); x1 = rd16(b + 8); y1 = rd16(b + 10);
    pitch = rd16(b + 66);
    if (x1 - x0 + 1 <= 0 || y1 - y0 + 1 <= 0) return 0;
    W = (uint32_t)(x1 - x0 + 1);
    H = (uint32_t)(y1 - y0 + 1);
    if (!(W <= pitch && H <= 4096) || W > FRAME_MAXW || pitch * H > (16u << 20)) return 0;
    need = pitch * H;
    /* a run may overshoot the last line by up to 63 bytes */
    if (!(line = (unsigned char*)malloc(need + 64))) return 0;
    while (got < need) {
        unsigned c;
        if (pos >= n) { free(line); return 0; }
        c = b[pos++];
        if (c >= 0xC0) {
            if (pos >= n) { free(line); return 0; }
            memset(line + got, b[pos++], c & 0x3F);
            got += c & 0x3F;
        } else line[got++] = (unsigned char)c;
    }
    if (!(px = (unsigned char*)malloc(W * H))) { free(line); return 0; }
    for (r = 0; r < H; r++) memcpy(px + r * W, line + r * pitch, W);
    free(line);
    *w = (int)W;
    *h = (int)H;
    *out = px;
    return 1;
}

/* The picture of one button by the spec's picture rule (decision 5), and the rect the engine gives
   it (`rect` in, the resized one out).
   Step 1 is the engine's: a slot the download fill wrote, or a gadget whose COMMON says `gaffile=1`
   (both set +0xB4 bit 0: 0x41AEAE, 0x4AD4C1), takes <anims>\<NAME>_gadget.GAF's sequence <NAME>
   with no fallback (0x4A83E2..0x4A84D8); any other its name in the screen's bank, then in
   commongui. For an order, tab or pager the generic commongui picture is also step 1 (it is what
   the game draws, with the gadget's text on it). For a build button it is not: 2. the unit's
   download picture, 3. <unitpics>\<UNIT>.PCX, its pixels taken as palette indices as an 8-bit blit
   takes them [INFERRED], 4. a labelled button.
   The rect: the engine resizes a button its stage pass pictured to that frame's size
   (0x4A8DCB..0x4A8DF3); a slot the download fill wrote keeps the size its first name was given; a
   `gaffile` gadget and a scroll arrow keep the file's; and so does a build button only the generic
   picture would picture (tagpu_wikiasset.h, the departures). */
static void gadget_picture(SIDE* S, const SREC* g, int role, const char* unit, const char* bank, SPIC* p, long long rect[4])
{
    SENG first, orig;
    int has_first = 0, k;
    memset(p, 0, sizeof *p);
    p->from = -1;
    p->key = -1;
    p->attribs = g->attribs;
    if (!(g->attr & ATTR_SCROLL) && !g->gaffile) has_first = engine_picture(S, g, g->name, bank, &first);
    if (g->filled) {
        if (!(g->attr & ATTR_SCROLL) && engine_picture(S, g, g->orig, bank, &orig)) {
            if (orig.how != WIKISIDE_FROM_FALLBACK) {
                const SFRAME* f0 = &S->fr[orig.q.f[orig.base]];
                rect[2] = f0->w;
                rect[3] = f0->h;
            }
            free(orig.q.f);
        }
        if (has_first) free(first.q.f);
        has_first = 0;
    }
    if (g->filled || g->gaffile) {
        char gn[SNAME + 8];
        SSEQ q;
        int bk;
        snprintf(gn, sizeof gn, "%s_gadget", g->name);
        if ((bk = side_bank(S, gn)) >= 0 && side_seq(S, bk, g->name, &q)) {
            pic_from_bank(S, p, WIKISIDE_FROM_DOWNLOAD, bk, g->name);
            pic_frames(p, &q, 0, g->attr);
            free(q.f);
        }
    } else if (has_first && (first.how != WIKISIDE_FROM_FALLBACK || role != WIKI_ROLE_BUILD)) {
        const SFRAME* f0 = &S->fr[first.q.f[first.base]];
        pic_from_bank(S, p, first.how, first.bank, first.seq);
        pic_frames(p, &first.q, first.base, g->attr);
        if (first.how == WIKISIDE_FROM_FALLBACK) scopy(p->text, g->text, sizeof p->text);
        rect[2] = f0->w;
        rect[3] = f0->h;
    }
    if (has_first) free(first.q.f);
    if (p->from < 0 && role == WIKI_ROLE_BUILD && unit[0]) {
        char gn[SNAME + 8];
        SSEQ q;
        int bk;
        snprintf(gn, sizeof gn, "%s_gadget", unit);
        if ((bk = side_bank(S, gn)) >= 0 && side_seq(S, bk, unit, &q)) {
            pic_from_bank(S, p, WIKISIDE_FROM_DOWNLOAD, bk, unit);
            pic_frames(p, &q, 0, g->attr);
            free(q.f);
        } else {
            TAGPU_VFILE f;
            if (side_find(S, S->in->names.unitpics, unit, "PCX", &f)) {
                unsigned char *b = side_read(S, &f, CAP_PCX, unit), *px;
                char key[TAGPU_VFS_PATHMAX + SNAME + 8], lp[TAGPU_VFS_PATHMAX];
                int w, h, at;
                lowcpy(lp, f.path, sizeof lp);
                snprintf(key, sizeof key, "%s:%s:pcx", lp, tagpu_vfs_source(S->vfs, f.src));
                if (b && (at = frame_find(S, key)) < 0 && pcx_read(b, f.size, &w, &h, &px))
                    at = frame_add(S, key, w, h, 0, 0, -1, px);
                else if (b && at < 0)
                    side_rep(S, "note", "%s: not an 8-bit one-plane RLE PCX", unit, f.path);
                if (b && at >= 0) {
                    p->from = WIKISIDE_FROM_UNITPICS;
                    p->nf = 1;
                    p->f[0] = at;
                    p->src = f.src;
                    memcpy(p->path, f.path, sizeof p->path);
                }
                free(b);
            }
        }
    }
    if (p->from < 0) {
        p->from = WIKISIDE_FROM_LABEL;
        p->nf = 0;
        scopy(p->text, unit[0] ? unit : g->name, sizeof p->text);
    }
    if (p->nf) {
        const int tk = S->fr[p->f[0]].tkey;
        p->key = tk;
    }
    for (k = p->nf; k < 3; k++) p->f[k] = -1;
}

/* The role the build pages' handler 0x41AA00 gives a press of the button, in its order: the name
   cut to 16 characters (0x49FED0), then the case-sensitive substrings PREV, NEXT, ORDERS, BUILD of
   that cut name (strstr, 0x41AA75..0x41AB30) -- a pager or tab whatever else it names -- then a
   unit type by that cut name (0x488B10 at 0x41AA39, tested at 0x41AB5D: the press arms it, or
   queues a mobile one), whatever the commonattribs; then commonattribs bit 2 (a build button whose
   name is no type) and STOP; any other button is an order. `unit` gets the type's UnitName
   upper-cased, or "". */
static int gadget_role(const SIDE* S, const SREC* g, char* unit, size_t nunit)
{
    static const struct { const char* sub; int role; } subs[] = {
        { "PREV", WIKI_ROLE_PREV }, { "NEXT", WIKI_ROLE_NEXT }, { "ORDERS", WIKI_ROLE_ORDERS }, { "BUILD", WIKI_ROLE_BUILDTAB } };
    char press[NAME_PRESS + 1];
    const SUNIT* u;
    int k;
    unit[0] = 0;
    snprintf(press, sizeof press, "%s", g->name);
    for (k = 0; k < 4; k++) if (strstr(press, subs[k].sub)) return subs[k].role;
    if ((u = unit_get(S, press)) != NULL) {
        upcpy(unit, u->name, nunit);
        return WIKI_ROLE_BUILD;
    }
    if (g->common & 4u) return WIKI_ROLE_BUILD;
    return strstr(g->name, "STOP") ? WIKI_ROLE_STOP : WIKI_ROLE_ORDER;
}

/* THE GREYED PASS 0x41B045..0x41B096, which runs on a numbered page only, over records 0 .. N-1
   with N the header's +0xB6 = the sections less one (0x41B054, 0x41B094): a record of
   commonattribs bit 2 there is greyed exactly when its whole name is no type (0x488B10 at 0x41B07A;
   0x4A1200 writes record i), so the page's last record, and every record of page 0 and of a side's
   screen, keeps the file's grey (after the download fill). `n` is the page's record count. */
static int button_grey(const SIDE* S, const SREC* g, int page, int n)
{
    if (page > 0 && g->index < n - 1 && (g->common & 4u)) return !unit_get(S, g->name);
    return (int)g->grey;
}

/* the panel's picture (the id-0 stage case 0x4A84F2..0x4A8658): `panel=` in the screen's bank,
   then commongui, drawn once at its origin minus its hotspot (0x4B0230 with one frame); else
   commongui BackTile with its hotspots zeroed, drawn once when it has one frame and as a 3 x 3
   nine-slice over the panel when it has more (0x4B0344..0x4B0439), composed here into one frame */
static void panel_picture(SIDE* S, SSCREEN* c, const SREC* hd, const char* bank)
{
    const int banks[2] = { side_bank(S, bank), S->common };
    SSEQ q;
    int k;
    c->panel = -1;
    for (k = 0; k < 2; k++)
        if (hd->panel[0] && side_seq(S, banks[k], hd->panel, &q)) {
            const SFRAME* f = &S->fr[q.f[0]];
            char up[SNAME];
            upcpy(up, hd->panel, sizeof up);
            c->panel = q.f[0];
            c->phx = f->hx;
            c->phy = f->hy;
            c->pkey = f->tkey;
            snprintf(c->pfrom, sizeof c->pfrom, "%s %s", k ? "commongui" : "page", up);
            memcpy(c->ppath, S->bank[banks[k]].path, sizeof c->ppath);
            c->psrc = S->bank[banks[k]].src;
            free(q.f);
            return;
        }
    if (!side_seq(S, S->common, "BackTile", &q)) return;
    c->phx = c->phy = 0;
    memcpy(c->ppath, S->bank[S->common].path, sizeof c->ppath);
    c->psrc = S->bank[S->common].src;
    if (q.n == 1) {
        c->panel = q.f[0];
        c->pkey = S->fr[q.f[0]].tkey;
        scopy(c->pfrom, "commongui BACKTILE", sizeof c->pfrom);
    } else {
        const int W = (int)hd->w, H = (int)hd->h, tw = S->fr[q.f[0]].w, th = S->fr[q.f[0]].h;
        const int keyc = S->fr[q.f[0]].tkey;
        int rows[FRAME_MAXH + 2], cols[FRAME_MAXW + 2], nr = 0, nc = 0, ri, ci;
        char key[48];
        unsigned char* px;
        /* the rows the tiles start at: every th from 0 while a tile still fits, then one more ending
           at the panel's edge; the columns the same way (0x4B03C3..0x4B03CE, 0x4B03E9..0x4B03F0) */
        if (th) { int y; for (y = 0; y <= (H - th > 1 ? H - th : 1) && nr < FRAME_MAXH; y += th) rows[nr++] = y; }
        else rows[nr++] = 0;
        if (rows[nr - 1] + th < H) rows[nr++] = H - th;
        if (tw) { int x; for (x = 0; x <= (W - tw > 1 ? W - tw : 1) && nc < FRAME_MAXW; x += tw) cols[nc++] = x; }
        else cols[nc++] = 0;
        if (cols[nc - 1] + tw < W) cols[nc++] = W - tw;
        snprintf(key, sizeof key, "backtile:%dx%d", W, H);
        if ((c->panel = frame_find(S, key)) < 0) {
            if (!(px = (unsigned char*)malloc((size_t)(W && H ? W * H : 1)))) { S->failed = 1; free(q.f); return; }
            memset(px, keyc, (size_t)(W * H));
            for (ri = 0; ri < nr; ri++) {
                const int rc = ri == 0 ? 0 : ri == nr - 1 ? 6 : 3;
                for (ci = 0; ci < nc; ci++) {
                    const int cc = ci == 0 ? 0 : ci == nc - 1 ? 2 : 1;
                    const SFRAME* f = &S->fr[q.f[rc + cc < q.n - 1 ? rc + cc : q.n - 1]];
                    int r, cx;
                    for (r = 0; r < f->h; r++) {
                        if (rows[ri] + r < 0 || rows[ri] + r >= H) continue;
                        for (cx = 0; cx < f->w; cx++) {
                            const unsigned char v = f->px[r * f->w + cx];
                            if (v != f->tkey && cols[ci] + cx >= 0 && cols[ci] + cx < W)
                                px[(rows[ri] + r) * W + cols[ci] + cx] = v;
                        }
                    }
                }
            }
            c->panel = frame_add(S, key, W, H, 0, 0, keyc, px);
        }
        c->pkey = keyc;
        scopy(c->pfrom, "commongui BACKTILE nine-slice", sizeof c->pfrom);
    }
    free(q.f);
}

/* the screen `name` among the live ones, or -1 */
static int screen_at(const SIDE* S, const char* name)
{
    int i;
    for (i = 0; i < S->nscr; i++) if (S->scr[i].live && !strcmp(S->scr[i].name, name)) return i;
    return -1;
}

static int live_screens(const SIDE* S)
{
    int i, n = 0;
    for (i = 0; i < S->nscr; i++) n += S->scr[i].live;
    return n;
}

/* 0x41ACE0's fill: each entry of the type with MENU - 1 == page (MENU zero-extended from its byte,
   0x41AE48) writes gadget BUTTON + 4 (BUTTON zero-extended from its byte too: stored with
   `mov BYTE PTR [..+0x7], al` at 0x42DEE7, read with `mov al, BYTE PTR [..+0x7]` over a cleared eax
   at 0x41AE57, DISASSEMBLED): the unit's name, commonattribs 4, not greyed, the `_gadget`
   picture; later entries overwrite. BOUND: THE ENGINE NEVER BOUNDS BUTTON + 4 against the gadget
   count (0x41AE5E..0x41AE73 indexes ControlsAry with it); here an entry whose slot is no gadget of
   the page is skipped and reported. */
static void fill_downloads(SIDE* S, SREC* r, int n, int page, const int* mine, int nmine, const char* name)
{
    int i;
    for (i = 0; i < nmine; i++) {
        const SENTRY* e = &S->dl[mine[i]];
        const long long slot = (e->button & 0xFF) + 4;
        SREC* g;
        if ((int)(e->menu & 0xFFu) - 1 != page) continue;
        if (slot < 0 || slot >= n || !r[slot].ok) {
            char el[2 * SNAME + 16];
            snprintf(el, sizeof el, "%s download %s", name, e->unitname);
            side_rep(S, "refused", "%s: BUTTON %lld names gadget %lld, which the page does not have", el,
                     S->dlfile[e->file], e->button, slot);
            continue;
        }
        g = &r[slot];
        memcpy(g->orig, g->name, sizeof g->orig);
        scopy(g->name, e->unitname, sizeof g->name);
        g->common = 4;
        g->grey = 0;
        g->filled = 1;
    }
}

/* One screen of the layout and its art, set under `name` (a live screen of that name is replaced
   in place). Gadgets are the buttons (id 1) in file order; the panel (id 0), labels (id 5) and fonts
   (id 7) are the art's. `page` is the build page number (the greyed pass runs on page != 0), -1 for
   a side's screen. 0 when the screen is refused. */
static int screen_build(SIDE* S, const char* name, const char* file, int page, int hide_pagers,
                        const int* mine, int nmine)
{
    TAGPU_VFILE f;
    SREC* r;
    SSCREEN* c;
    int n = 0, i, at;
    const SREC* hd;
    if (strlen(name) >= WIKI_SIDE_NAME) {
        side_rep(S, "refused", "a screen name of %d characters, past the layout's", name, (int)strlen(name));
        return 0;
    }
    if (!(r = screen_read(S, file, &n, &f))) return 0;
    if (page >= 0 && nmine) fill_downloads(S, r, n, page, mine, nmine, name);
    hd = &r[0];
    if (hd->x != 0) side_rep(S, "note", "the panel is at x %lld; drawn at 0", name, hd->x);
    if (!(hd->w >= 1 && hd->w <= WIKI_SIDE_W && hd->h >= 1 && hd->y >= 0 && hd->y + hd->h <= WIKI_SIDE_ROWS)) {
        side_rep(S, "refused", "a %lld x %lld panel at row %lld is outside the column", name, hd->w, hd->h, hd->y);
        free(r);
        return 0;
    }
    if ((at = screen_at(S, name)) < 0) {
        if (!side_grow(S, (void**)&S->scr, &S->cscr, S->nscr + 1, sizeof *S->scr)) { free(r); return 0; }
        at = S->nscr++;
    }
    c = &S->scr[at];
    memset(c, 0, sizeof *c);
    c->live = 1;
    scopy(c->name, name, sizeof c->name);
    scopy(c->file, file, sizeof c->file);
    memcpy(c->path, f.path, sizeof c->path);
    c->src = f.src;
    c->y = (int)hd->y;
    c->w = (int)hd->w;
    c->h = (int)hd->h;
    for (i = 1; i < n && !S->failed; i++) {
        const SREC* g = &r[i];
        char unit[WIKI_SIDE_NAME], el[2 * SNAME + 16];
        long long rect[4], cw, ch, x0, y0;
        int role, grey;
        SPIC pic;
        WIKIGADGET* lg;
        if (!g->ok) continue;
        snprintf(el, sizeof el, "%s/%s#%d", name, g->name, g->index);
        if (g->id == 5) {
            if (c->nlabel == WIKISIDE_LABELS) { side_rep(S, "refused", "past %d labels", el, WIKISIDE_LABELS); continue; }
            c->label[c->nlabel].x = (int)g->x; c->label[c->nlabel].y = (int)g->y;
            c->label[c->nlabel].w = (int)g->w; c->label[c->nlabel].h = (int)g->h;
            scopy(c->label[c->nlabel].text, g->text, WIKISIDE_TEXT);
            c->nlabel++;
            continue;
        }
        if (g->id != 1) continue;
        role = gadget_role(S, g, unit, sizeof unit);
        grey = button_grey(S, g, page, n);
        rect[0] = g->x; rect[1] = g->y; rect[2] = g->w; rect[3] = g->h;
        gadget_picture(S, g, role, unit, file, &pic, rect);
        x0 = rect[0] > 0 ? rect[0] : 0;
        y0 = rect[1] > 0 ? rect[1] : 0;
        cw = (rect[0] + rect[2] < c->w ? rect[0] + rect[2] : c->w) - x0;
        ch = (rect[1] + rect[3] < c->h ? rect[1] + rect[3] : c->h) - y0;
        if (cw < 1 || ch < 1) {
            side_rep(S, "refused", "the rect [%lld, %lld, %lld, %lld] is outside the screen", el, rect[0], rect[1], rect[2], rect[3]);
            continue;
        }
        if (x0 != rect[0] || y0 != rect[1] || cw != rect[2] || ch != rect[3])
            side_rep(S, "note", "the rect [%lld, %lld, %lld, %lld] is clipped to the screen", el, rect[0], rect[1], rect[2], rect[3]);
        /* WIKI_SIDE_GADGETS: the layout's per-screen gadget table; a button past it is left out */
        if (c->ng == WIKI_SIDE_GADGETS) { side_rep(S, "refused", "past %d buttons", el, WIKI_SIDE_GADGETS); continue; }
        if (!g->name[0] || strlen(g->name) >= WIKI_SIDE_NAME) {
            side_rep(S, "refused", "a name the layout cannot hold (%d characters)", el, (int)strlen(g->name));
            continue;
        }
        lg = &c->g[c->ng];
        memset(lg, 0, sizeof *lg);
        scopy(lg->name, g->name, sizeof lg->name);
        lg->role = role;
        scopy(lg->unit, unit, sizeof lg->unit);
        lg->grey = grey;
        lg->hidden = !g->active || (hide_pagers && (role == WIKI_ROLE_PREV || role == WIKI_ROLE_NEXT));
        lg->x = (int)x0; lg->y = (int)y0; lg->w = (int)cw; lg->h = (int)ch;
        c->pic[c->ng] = pic;
        c->ng++;
    }
    panel_picture(S, c, hd, file);
    free(r);
    return !S->failed;
}

/* The def load's page count (0x42D800..0x42D8CB, 0x42DF60..0x42DFB4) and each page's file: page 0
   only when <NAME>0.GUI exists; def+0x22E is the first missing index of the <NAME>1, 2, ... probe
   (or 1 with only <NAME>0), raised to every download entry's MENU byte; the numbered pages are
   1 .. def+0x22E - 1. A page whose file is missing is the PLAYER's side's <prefix>DL. Returns the
   last numbered page, -1 for a type with no page (or refused). */
static int type_pages(SIDE* S, const char* tname, const int* mine, int nmine, int* zero)
{
    TAGPU_VFILE f;
    char nm[SNAME + 8];
    int n = 1, count, last, i;
    snprintf(nm, sizeof nm, "%s0", tname);
    *zero = side_find(S, S->in->names.gui, nm, "GUI", &f);
    for (;;) {
        snprintf(nm, sizeof nm, "%s%d", tname, n);
        if (n >= 256 || !side_find(S, S->in->names.gui, nm, "GUI", &f)) break;
        n++;
    }
    count = n > 1 ? n : *zero ? 1 : 0;
    for (i = 0; i < nmine; i++)
        if ((int)(S->dl[mine[i]].menu & 0xFFu) > count) count = (int)(S->dl[mine[i]].menu & 0xFFu);
    last = count >= 2 ? count - 1 : 0;
    if (!count || (!last && !*zero)) return -1;
    if (last > WIKI_SIDE_PAGES) {
        char el[SNAME + 8];
        snprintf(el, sizeof el, "type %s", tname);
        side_rep(S, "refused", "%d numbered pages, more than %d", el, last, WIKI_SIDE_PAGES);
        return -1;
    }
    return last;
}

/* ---- the cast rule (gfx.py `side_casting`; tagpu_wikiasset.h, THE CASTING) ----
   research/notes/ingame-wiki.md, the owner's decisions 14 and 20. Units are SIDE.unit indices
   throughout, so a name the layout cannot hold still has its place. */

static const char* const CAST_NAME[WIKI_NCAST] = { "builder", "advbuilder", "mex", "moho", "geo", "walker", "tower", "wall",
                                                   "solar" };

static int is_mobile(const SUNIT* u) { return u->bmcode != 0 && u->maxvelocity > 0; }
static int is_structure(const SUNIT* u) { return !u->bmcode && u->fx > 0 && u->fz > 0; }
static int is_builder(const SUNIT* u) { return (u->builder & 1u) && u->workertime > 0; }
/* neither a flyer nor bound to water; an FBI without MinWaterDepth is a land unit */
static int is_land(const SUNIT* u) { return !u->canfly && (!u->has_mwd || u->mwd <= 0); }
static int is_extractor(const SUNIT* u) { return is_structure(u) && u->extracts > 0; }
/* a G cell in the yard ties a building to a vent: 0x47D2E0 refuses a yard holding G unless some G
   cell stands on a geothermal feature (0x47D684..0x47D711, refusal 0x47D80F; DISASSEMBLED) */
static int is_geothermal(const SUNIT* u) { return is_structure(u) && !is_extractor(u) && u->has_g; }
/* a land building that is neither a resource building nor a builder: a tower, a wall, a plant */
static int is_plain(const SUNIT* u)
{
    return is_structure(u) && is_land(u) && !is_extractor(u) && !is_geothermal(u) && !(u->builder & 1u);
}
/* what the building adds to the energy income: a solar collector's is a negative EnergyUse */
static double net_energy(const SUNIT* u) { return u->energymake - u->energyuse; }
static int land_mex(const SUNIT* u) { return is_extractor(u) && is_land(u); }
static int mobile_builder(const SUNIT* u) { return is_mobile(u) && is_builder(u) && is_land(u); }

/* a menu: unit indices in (page, gadget) order, each once */
typedef struct { int done, n, cap; int* u; } SMENU;

static int menu_has(const SMENU* m, int u)
{
    int i;
    for (i = 0; i < m->n; i++) if (m->u[i] == u) return 1;
    return 0;
}

/* the download entries of type `u` into `mine` (at most S->ndl) */
static int dl_mine(const SIDE* S, const SUNIT* u, int* mine)
{
    int k, n = 0;
    for (k = 0; k < S->ndl; k++) if (ieq(S->dl[k].unitmenu, u->name)) mine[n++] = k;
    return n;
}

/* gfx.py `menu_of`: menu(T) for the side whose prefix is `prefix`, cached in `cache` (one SMENU a
   unit, shared by the sides of one prefix): the units of T's build buttons on its numbered pages
   as the layout reads them -- the page probe, else <prefix>DL, the download fill, the greyed pass
   -- hidden and greyed buttons left out: a button counts when it is a gadget (id 1), active, not
   greyed (button_grey) and its role a build of a type (gadget_role). A page the layout would
   refuse leaves T with no menu, as the layout then holds no page of T. Nothing is reported. */
static const SMENU* menu_of(SIDE* S, SMENU* cache, int t, const char* prefix, int* mine)
{
    SMENU* m = &cache[t];
    const SUNIT* u = &S->unit[t];
    int nmine, zero, last, p, ok = 1;
    if (m->done) return m;
    m->done = 1;
    S->quiet++;
    nmine = dl_mine(S, u, mine);
    last = type_pages(S, u->name, mine, nmine, &zero);
    for (p = 1; p <= last && ok && !S->failed; p++) {
        TAGPU_VFILE f;
        char file[SNAME + 8];
        SREC* r;
        int n = 0, i;
        snprintf(file, sizeof file, "%s%d", u->name, p);
        if (!side_find(S, S->in->names.gui, file, "GUI", &f)) snprintf(file, sizeof file, "%sDL", prefix);
        if (!(r = screen_read(S, file, &n, &f))) { ok = 0; break; }
        if (!(r[0].w >= 1 && r[0].w <= WIKI_SIDE_W && r[0].h >= 1 && r[0].y >= 0 && r[0].y + r[0].h <= WIKI_SIDE_ROWS)) {
            free(r);
            ok = 0;
            break;
        }
        fill_downloads(S, r, n, p, mine, nmine, file);
        for (i = 1; i < n; i++) {
            char unit[WIKI_SIDE_NAME];
            const SUNIT* x;
            int at;
            if (!r[i].ok || r[i].id != 1 || !r[i].active || button_grey(S, &r[i], p, n) ||
                gadget_role(S, &r[i], unit, sizeof unit) != WIKI_ROLE_BUILD || !unit[0] || !(x = unit_get(S, unit)))
                continue;
            at = (int)(x - S->unit);
            if (menu_has(m, at)) continue;
            if (!side_grow(S, (void**)&m->u, &m->cap, m->n + 1, sizeof *m->u)) break;
            m->u[m->n++] = at;
        }
        free(r);
    }
    if (!ok) m->n = 0;
    S->quiet--;
    return m;
}

/* one side's walk and casts */
typedef struct {
    SIDE*       S;
    SMENU*      cache;
    const char* prefix;
    int*        mine;
} SWALK;

static const SMENU* menu(SWALK* W, int t) { return menu_of(W->S, W->cache, t, W->prefix, W->mine); }

static int menu_holds(SWALK* W, int t, int (*pred)(const SUNIT*))
{
    const SMENU* m = menu(W, t);
    int i;
    for (i = 0; i < m->n; i++) if (pred(&W->S->unit[m->u[i]])) return 1;
    return 0;
}

/* a pick and its reason */
typedef struct { int pick[WIKI_NCAST]; char why[WIKI_NCAST][200]; } SCASTING;

/* A cast WIKICASTDEF cannot hold (tagpu_wikiasset.h, where this departs from gfx.py), or NULL:
   tagpu_wikidata_casting's bounds, which the simulation's binding rests on */
static const char* cast_unfit(const SUNIT* u)
{
    const double f[6] = { u->buildtime, u->workertime, u->builddistance, u->maxvelocity, u->extracts, u->energymake };
    const size_t n = strlen(u->name);
    int i;
    if (n >= WIKI_ID_MAX || strspn(u->name, "0123456789") == n) return "a UnitName the casting cannot hold";
    for (i = 0; i < 6; i++) if (!(f[i] >= 0 && f[i] < INFINITY)) return "a figure that is not a finite number >= 0";
    if (!(u->buildtime > 0)) return "a BuildTime that is not above 0";
    return NULL;
}

/* the plain casts' own tests: tower, wall, solar */
static int plain_test(int c, const SUNIT* x)
{
    if (c == WIKI_CAST_TOWER) return x->weapon1 != 0;
    if (c == WIKI_CAST_WALL) return !x->weapon1 && x->fort;
    return net_energy(x) > 0;
}

/* unit index t is already some cast's pick */
static int picked(const SCASTING* C, int t)
{
    int c;
    for (c = 0; c < WIKI_NCAST; c++) if (C->pick[c] == t) return 1;
    return 0;
}

/* gfx.py `side_casting` over the units of SIDE, `commander` the side's root. BOUND: the walk's
   order holds each unit once (depth -1 marks the unmet), so it is at most S->nunit long. */
static void side_casting(SWALK* W, const char* commander, int* order, int* depth, SCASTING* C)
{
    SIDE* S = W->S;
    const SUNIT* U = S->unit;
    const SUNIT* root = unit_get(S, commander);
    int n = 0, i, k, c, b = -1, mx = -1, a = -1;
    for (c = 0; c < WIKI_NCAST; c++) { C->pick[c] = -1; C->why[c][0] = 0; }
    if (!root) {
        for (c = 0; c < WIKI_NCAST; c++)
            snprintf(C->why[c], sizeof C->why[c], "the side's commander %s has no FBI", commander[0] ? commander : "(none)");
        return;
    }
    for (i = 0; i < S->nunit; i++) depth[i] = -1;
    order[n++] = (int)(root - U);
    depth[order[0]] = 0;
    for (i = 0; i < n && !S->failed; i++) {
        const SMENU* m = menu(W, order[i]);
        for (k = 0; k < m->n; k++)
            if (depth[m->u[k]] < 0) { depth[m->u[k]] = depth[order[i]] + 1; order[n++] = m->u[k]; }
    }
    for (i = 1; i < n && b < 0; i++) if (mobile_builder(&U[order[i]]) && menu_holds(W, order[i], land_mex)) b = order[i];
    C->pick[WIKI_CAST_BUILDER] = b;
    if (b < 0) {
        snprintf(C->why[WIKI_CAST_BUILDER], 200, "no mobile land builder below the commander has a land extractor on its menu");
        for (c = 1; c < WIKI_NCAST; c++) snprintf(C->why[c], 200, "no builder");
    } else {
        const SMENU* mb = menu(W, b);
        int geo = -1, maker = -1, w = -1, mo = -1;
        double top;
        snprintf(C->why[WIKI_CAST_BUILDER], 200, "the first met (depth %d) mobile land builder whose menu holds a land extractor", depth[b]);
        /* the strongest, the earlier on an equal value */
        for (k = 0; k < mb->n; k++) {
            const SUNIT* x = &U[mb->u[k]];
            if (land_mex(x) && (mx < 0 || x->extracts > U[mx].extracts)) mx = mb->u[k];
            if (is_geothermal(x) && is_land(x) && (geo < 0 || net_energy(x) > net_energy(&U[geo]))) geo = mb->u[k];
        }
        C->pick[WIKI_CAST_MEX] = mx;
        snprintf(C->why[WIKI_CAST_MEX], 200, "the strongest land extractor on the builder's menu (extractsmetal %g)", U[mx].extracts);
        C->pick[WIKI_CAST_GEO] = geo;
        if (geo >= 0) snprintf(C->why[WIKI_CAST_GEO], 200, "the land geothermal plant of the greatest net energy (%g) on the builder's menu", net_energy(&U[geo]));
        else snprintf(C->why[WIKI_CAST_GEO], 200, "no land geothermal plant on the builder's menu");
        top = U[mx].extracts;
        for (i = 1; i < n && a < 0; i++) {
            const SMENU* m;
            if (order[i] == b || !mobile_builder(&U[order[i]])) continue;
            m = menu(W, order[i]);
            for (k = 0; k < m->n; k++) if (land_mex(&U[m->u[k]]) && U[m->u[k]].extracts > top) { a = order[i]; break; }
        }
        C->pick[WIKI_CAST_ADVBUILDER] = a;
        if (a >= 0) {
            const SMENU* ma = menu(W, a);
            snprintf(C->why[WIKI_CAST_ADVBUILDER], 200, "the first met (depth %d) mobile land builder, after the builder, whose menu holds a land extractor stronger than the mex", depth[a]);
            for (k = 0; k < ma->n; k++) {
                const SUNIT* x = &U[ma->u[k]];
                if (land_mex(x) && x->extracts > top && (mo < 0 || x->extracts > U[mo].extracts)) mo = ma->u[k];
            }
            C->pick[WIKI_CAST_MOHO] = mo;
            snprintf(C->why[WIKI_CAST_MOHO], 200, "the strongest land extractor on the advbuilder's menu (extractsmetal %g)", U[mo].extracts);
        } else {
            snprintf(C->why[WIKI_CAST_ADVBUILDER], 200, "no other mobile land builder has a land extractor stronger than the mex on its menu");
            snprintf(C->why[WIKI_CAST_MOHO], 200, "no advbuilder");
        }
        /* the first met unit whose menu holds the builder: one exists, since the builder was met
           on some unit's menu */
        for (i = 0; i < n && maker < 0; i++) if (menu_has(menu(W, order[i]), b)) maker = order[i];
        if (maker >= 0) {
            const SMENU* mm = menu(W, maker);
            for (k = 0; k < mm->n && w < 0; k++) {
                const SUNIT* x = &U[mm->u[k]];
                if (is_mobile(x) && !is_builder(x) && is_land(x)) w = mm->u[k];
            }
            snprintf(C->why[WIKI_CAST_WALKER], 200, w >= 0 ? "the first mobile land non-builder on the menu of %s, the first met unit that builds the builder"
                                                         : "no mobile land non-builder on the menu of %s, the first met unit that builds the builder", U[maker].name);
        }
        C->pick[WIKI_CAST_WALKER] = w;
        /* the last three casts share one list: each skips a unit an earlier cast took */
        {
            static const struct { int c; const char* what; } plain[3] = {
                { WIKI_CAST_TOWER, "with a Weapon1" }, { WIKI_CAST_WALL, "with no Weapon1 and TEDClass FORT" },
                { WIKI_CAST_SOLAR, "of a positive net energy" } };
            int j;
            for (j = 0; j < 3; j++) {
                const int pc = plain[j].c;
                for (k = 0; k < mb->n && C->pick[pc] < 0; k++) {
                    const SUNIT* x = &U[mb->u[k]];
                    if (is_plain(x) && plain_test(pc, x) && !picked(C, mb->u[k])) C->pick[pc] = mb->u[k];
                }
                snprintf(C->why[pc], 200, "%s plain land building on the builder's menu %s",
                         C->pick[pc] >= 0 ? "the first" : "no", plain[j].what);
            }
        }
    }
    /* every cast's unit distinct whatever the tests: a pick repeating an earlier cast's is unbound */
    for (c = 1; c < WIKI_NCAST; c++) {
        int d;
        for (d = 0; d < c && C->pick[c] >= 0; d++)
            if (C->pick[d] == C->pick[c]) {
                snprintf(C->why[c], 200, "%s is already the %s", U[C->pick[c]].name, CAST_NAME[d]);
                C->pick[c] = -1;
            }
    }
    /* WIKI_FP_MAX: the G rows' width; then what WIKICASTDEF cannot hold */
    for (c = 0; c < WIKI_NCAST; c++) {
        const int t = C->pick[c];
        const char* unfit;
        if (t < 0) continue;
        if (!(U[t].fx >= 1 && U[t].fx <= WIKI_FP_MAX && U[t].fz >= 1 && U[t].fz <= WIKI_FP_MAX)) {
            snprintf(C->why[c], 200, "%s's footprint %lld x %lld is not within 1..%d a side", U[t].name, U[t].fx, U[t].fz, WIKI_FP_MAX);
            C->pick[c] = -1;
        } else if ((unfit = cast_unfit(&U[t])) != NULL) {
            snprintf(C->why[c], 200, "%s has %s", U[t].name, unfit);
            C->pick[c] = -1;
        }
    }
}

/* Windows-1252's 0x80..0x9F as Unicode, 0 for the five bytes it leaves undefined, which become
   `?` (gfx.py `display_name`); every other byte is its own code point */
static const uint16_t CP1252_HI[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

static unsigned cp1252(unsigned char c)
{
    if (c < 0x80 || c > 0x9F) return c;
    return CP1252_HI[c - 0x80] ? CP1252_HI[c - 0x80] : '?';
}

/* THE DISPLAY NAME as code points: the title's bytes through Windows-1252, or with none the
   UnitName, whose bytes gfx.py reads as Latin-1. At most 63 code points (the name's SNAME). */
static int title_points(const SUNIT* u, unsigned* cp)
{
    int i, n = 0;
    if (u->ntitle) for (i = 0; i < u->ntitle; i++) cp[n++] = cp1252(u->title[i]);
    else for (i = 0; u->name[i] && i < SNAME; i++) cp[n++] = (unsigned char)u->name[i];
    return n;
}

/* the cast's WIKICASTDEF from its unit, whose fit cast_unfit and the footprint bound have passed.
   BOUND: the title is at most 31 code points of at most 3 UTF-8 bytes (U+20AC and U+2122 are the
   widest), 93 bytes and the terminator, WIKI_TITLE_MAX. */
static void cast_fill(const SUNIT* u, uint16_t builds, WIKICASTDEF* k)
{
    unsigned cp[SNAME];
    int n = title_points(u, cp), i;
    size_t at = 0;
    memset(k, 0, sizeof *k);
    k->bound = 1;
    upcpy(k->type, u->name, sizeof k->type);
    for (i = 0; i < n && at + 4 <= sizeof k->title; i++) {
        const unsigned c = cp[i];
        if (c < 0x80) k->title[at++] = (char)c;
        else if (c < 0x800) { k->title[at++] = (char)(0xC0 | (c >> 6)); k->title[at++] = (char)(0x80 | (c & 0x3F)); }
        else {
            k->title[at++] = (char)(0xE0 | (c >> 12));
            k->title[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
            k->title[at++] = (char)(0x80 | (c & 0x3F));
        }
    }
    k->title[at] = 0;
    k->fx = (int)u->fx;
    k->fz = (int)u->fz;
    k->build_time = u->buildtime;
    k->worker_time = u->workertime;
    k->build_distance = u->builddistance;
    k->max_velocity = u->maxvelocity;
    k->extracts = u->extracts;
    k->energy_make = u->energymake;
    k->canstop = u->canstop;
    k->activate_when_built = u->activate_when_built;
    k->vent = u->has_g;
    memcpy(k->g, u->g, sizeof k->g);
    k->builds = builds;
}

/* a double as JSON, the shortest text that reads back as the same double */
static void sb_num(SB* b, double v)
{
    char t[40];
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(t, sizeof t, "%.*g", p, v);
        if (strtod(t, NULL) == v) break;
    }
    sb_raw(b, "%s", t);
}

/* one side's casting in the pack's form (gfx.py `side_pack`'s `casting` list) */
static void casting_json(SB* b, const SIDE* S, const SSIDE* sd, const SCASTING* C, const WIKICASTING* K)
{
    int c, d, i, n;
    sb_raw(b, "{\"side\":");
    sb_q(b, sd->name, 0);
    sb_raw(b, ",\"commander\":");
    sb_q(b, sd->commander, 0);
    sb_raw(b, ",\"casts\":{");
    for (c = 0; c < WIKI_NCAST; c++) {
        const WIKICASTDEF* k = &K->cast[c];
        sb_raw(b, "%s\"%s\":", c ? "," : "", CAST_NAME[c]);
        if (!k->bound) { sb_raw(b, "null"); continue; }
        {
            unsigned cp[SNAME];
            char t[8];
            n = title_points(&S->unit[C->pick[c]], cp);
            sb_raw(b, "{\"type\":");
            sb_q(b, k->type, 0);
            sb_raw(b, ",\"title\":\"");
            for (i = 0; i < n; i++) {
                if (cp[i] == '"' || cp[i] == '\\') { t[0] = '\\'; t[1] = (char)cp[i]; sb_add(b, t, 2); }
                else if (cp[i] < 0x20 || cp[i] >= 0x7F) { snprintf(t, sizeof t, "\\u%04x", cp[i]); sb_add(b, t, 6); }
                else { t[0] = (char)cp[i]; sb_add(b, t, 1); }
            }
        }
        sb_raw(b, "\",\"footprint\":[%d,%d],\"buildTime\":", k->fx, k->fz);
        sb_num(b, k->build_time);
        sb_raw(b, ",\"workerTime\":");
        sb_num(b, k->worker_time);
        sb_raw(b, ",\"buildDistance\":");
        sb_num(b, k->build_distance);
        sb_raw(b, ",\"maxVelocity\":");
        sb_num(b, k->max_velocity);
        sb_raw(b, ",\"canstop\":%d,\"activatewhenbuilt\":%d,\"extracts\":", k->canstop, k->activate_when_built);
        sb_num(b, k->extracts);
        sb_raw(b, ",\"energyMake\":");
        sb_num(b, k->energy_make);
        sb_raw(b, ",\"vent\":%s,\"g\":[", k->vent ? "true" : "false");
        for (n = 0, i = 0; i < k->fx * k->fz; i++)
            if (k->g[i / k->fx] >> (i % k->fx) & 1u) sb_raw(b, "%s[%d,%d]", n++ ? "," : "", i % k->fx, i / k->fx);
        sb_raw(b, "],\"builds\":[");
        for (n = 0, d = 0; d < WIKI_NCAST; d++) if (k->builds >> d & 1u) sb_raw(b, "%s\"%s\"", n++ ? "," : "", CAST_NAME[d]);
        sb_raw(b, "]}");
    }
    sb_raw(b, "},\"report\":[");
    for (c = 0; c < WIKI_NCAST; c++) {
        sb_raw(b, "%s{\"cast\":\"%s\",\"type\":", c ? "," : "", CAST_NAME[c]);
        sb_q(b, K->cast[c].bound ? K->cast[c].type : NULL, 0);
        sb_raw(b, ",\"why\":");
        sb_q(b, C->why[c], 0);
        sb_raw(b, "}");
    }
    sb_raw(b, "]}");
}

/* ---- the colour tables (each formula DISASSEMBLED; the retail results equal the shipped
   PALETTE.ALP and a live read of the GUI table, MEASURED by the Q2 scout) ---- */

/* 0x4BA920: each entry's R+G+B and its index, exchange-sorted by the sum with a strict `>` (not a
   stable order: the loop is copied as it is) */
static void pal_sorted(const unsigned char* pal, int* sum, int* idx)
{
    int i, j;
    for (i = 0; i < 256; i++) { sum[i] = pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2]; idx[i] = i; }
    for (i = 0; i < 256; i++)
        for (j = i + 1; j < 256; j++)
            if (sum[i] > sum[j]) {
                int t = sum[i]; sum[i] = sum[j]; sum[j] = t;
                t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
}

/* 0x4BA9D0: the nearest entry by squared RGB distance among those whose sum is within 40 of the
   colour's, the first strict minimum; with none in the window, the entry where the walk stopped */
static int pal_nearest(const unsigned char* pal, const int* sum, const int* idx, int r, int g, int b)
{
    const int want = r + g + b;
    long best = 1000000000L;
    int hit = -1, k = 0;
    while (k < 256) {
        const unsigned char* c;
        long d;
        if (sum[k] < want - 40) { k++; continue; }
        if (sum[k] > want + 40) break;
        c = pal + 4 * idx[k];
        d = (long)(c[0] - r) * (c[0] - r) + (long)(c[1] - g) * (c[1] - g) + (long)(c[2] - b) * (c[2] - b);
        if (d < best) { best = d; hit = k; }
        k++;
    }
    return idx[hit >= 0 ? hit : k & 0xFF];
}

/* <palette>.ALP by the path rule, else 0x4BA750's generator: T[r][c] = r on the diagonal, else the
   nearest entry to the two colours' per-channel mean, floored. The mods' files are not the
   generator's output (MEASURED, Q2: 508 to 53 378 cells differ), so the file is what the game
   uses. */
static void alp_table(SIDE* S, const unsigned char* pal, const int* sum, const int* idx, unsigned char* alp,
                      char* from, size_t nfrom)
{
    TAGPU_VFILE f;
    int r, c;
    if (side_find(S, S->in->names.palettes, S->in->names.palette, "ALP", &f) && f.size >= 65536) {
        char why[200];
        if (tagpu_vfs_read(S->vfs, &f, 0, 65536, alp, why, sizeof why)) {
            snprintf(from, nfrom, "%s", f.path);
            return;
        }
        side_rep(S, "note", "%s: %s; the table is generated", "alp", f.path, why);
    }
    for (r = 0; r < 256; r++)
        for (c = 0; c < 256; c++)
            alp[r * 256 + c] = (unsigned char)(r == c ? r : pal_nearest(pal, sum, idx,
                (pal[4 * r] + pal[4 * c]) >> 1, (pal[4 * r + 1] + pal[4 * c + 1]) >> 1, (pal[4 * r + 2] + pal[4 * c + 2]) >> 1));
    from[0] = 0;
}

/* stable shelf packing, tallest first: tascene.shelf_pack's order, so the sheets are the same */
static int frame_taller(const void* a, const void* b)
{
    const SFRAME *x = *(const SFRAME* const*)a, *y = *(const SFRAME* const*)b;
    if (x->h != y->h) return y->h - x->h;
    return x < y ? -1 : x > y;
}

static void frame_json(SB* b, const SFRAME* f, int hx, int hy)
{
    sb_raw(b, "{\"w\":%d,\"h\":%d,\"hx\":%d,\"hy\":%d,\"fnv\":\"%08lx\"}", f->w, f->h, hx, hy,
           (unsigned long)fnv(FNV0, f->px, (size_t)f->w * (size_t)f->h));
}

static const char* const FROM_NAME[] = { "label", "page", "commongui", "fallback", "download", "unitpics" };
static const char* const ROLE_NAME[] = { "orders", "buildtab", "build", "prev", "next", "stop", "order" };

static WIKISIDEFRAME sheet_rect(const SFRAME* f, int hx, int hy)
{
    WIKISIDEFRAME r;
    r.sx = f->x; r.sy = f->y; r.w = f->w; r.h = f->h; r.hx = hx; r.hy = hy;
    return r;
}

static int frame_inside(const WIKISIDE* w, const WIKISIDEFRAME* q)
{
    return q->sx >= 0 && q->sy >= 0 && q->w >= 1 && q->h >= 1 && q->sx <= w->sheet_w - q->w && q->sy <= w->sheet_h - q->h;
}

static int side_frames_inside(const WIKISIDE* w)
{
    int s, k, i;
    for (k = 0; k < WIKI_SIDE_SIDES; k++) if (w->has_panelside[k] && !frame_inside(w, &w->panelside[k])) return 0;
    if (w->has_high && !frame_inside(w, &w->high)) return 0;
    for (k = 0; k < w->nradlogo; k++) if (!frame_inside(w, &w->radlogo[k])) return 0;
    for (s = 0; s < w->layout.nscreen; s++) {
        const WIKISIDESCREEN* o = &w->screen[s];
        if (o->has_panel && !frame_inside(w, &o->panel)) return 0;
        for (k = 0; k < w->layout.screen[s].ngadget; k++)
            for (i = 0; i < o->pic[k].nframe; i++) if (!frame_inside(w, &o->pic[k].frame[i])) return 0;
    }
    return 1;
}

/* a type of a side's layout: its pages' files (to tell whether another side's are the same) and
   the tag its screens' names carry */
typedef struct {
    char name[WIKI_SIDE_NAME], tag[8];
    int  side, last, zero;
    char file[WIKI_SIDE_PAGES + 1][SNAME + 8];
} STYPE;

/* gfx.py `type_pages`' list of files: page p's own <NAME><p>, else the side's <prefix>DL */
static void type_files(SIDE* S, const SUNIT* u, const char* prefix, STYPE* t)
{
    TAGPU_VFILE f;
    int p;
    for (p = t->zero ? 0 : 1; p <= t->last; p++) {
        snprintf(t->file[p], sizeof t->file[p], "%s%d", u->name, p);
        if (p > 0 && !side_find(S, S->in->names.gui, t->file[p], "GUI", &f)) snprintf(t->file[p], sizeof t->file[p], "%sDL", prefix);
    }
}

static int same_files(const STYPE* a, const STYPE* b)
{
    int p;
    if (a->zero != b->zero || a->last != b->last) return 0;
    for (p = a->zero ? 0 : 1; p <= a->last; p++) if (strcmp(a->file[p], b->file[p])) return 0;
    return 1;
}

/* a cast the rule took from a menu that the layout shows no live button for would lock its demo's
   build trip: reported, as gfx.py reports it */
static void casting_notes(SIDE* S, const SSIDE* sd, const WIKICASTING* K, const STYPE* types, int ntype, int side)
{
    int c, d, j, p, i;
    for (c = 0; c < WIKI_NCAST; c++) {
        const STYPE* t = NULL;
        if (!K->cast[c].bound) continue;
        for (j = 0; j < ntype; j++) if (types[j].side == side && !strcmp(types[j].name, K->cast[c].type)) t = &types[j];
        if (!t) continue;
        for (d = 0; d < WIKI_NCAST; d++) {
            int live = 0;
            if (!(K->cast[c].builds >> d & 1u)) continue;
            for (p = 1; p <= t->last && !live; p++) {
                char name[WIKI_SIDE_NAME + 16];
                int at;
                snprintf(name, sizeof name, "%s%d%s", t->name, p, t->tag);
                if ((at = screen_at(S, name)) < 0) continue;
                for (i = 0; i < S->scr[at].ng && !live; i++) {
                    const WIKIGADGET* g = &S->scr[at].g[i];
                    live = g->role == WIKI_ROLE_BUILD && !strcmp(g->unit, K->cast[d].type) && !g->grey && !g->hidden;
                }
            }
            if (!live) {
                char el[SNAME + 16];
                snprintf(el, sizeof el, "casting %s", sd->name);
                side_rep(S, "note", "the %s %s's pages show no live button for the %s %s", el, CAST_NAME[c], K->cast[c].type,
                         CAST_NAME[d], K->cast[d].type);
            }
        }
    }
}

const WIKISIDE* tagpu_wikiside_load(const WIKI3DIN* in, TAGPU_VFS* vfs, char** report)
{
    SIDE S;
    WIKILAYOUT* lay = NULL;
    WIKISIDE* out = NULL;
    SFRAME** order = NULL;
    TAGPU_VFILE f;
    char why[200], err[200], alp_from[TAGPU_VFS_PATHMAX];
    char gui_from[TAGPU_VFS_PATHMAX] = "", shd_from[TAGPU_VFS_PATHMAX] = "";
    unsigned char pal[1024], gui[256], seen_t[256], greyed[256], *alp = NULL, *sheet = NULL, *shd = NULL;
    int sum[256], idx[256], side_map[WIKI_SIDE_SIDES], kept[WIKI_SIDE_SIDES], i, k, sh = 0, has_gui = 0,
        has_greyed = 0, refused = 1, closed = 0, ntype = 0, nlay_side = 0, *mine = NULL, *walk = NULL,
        *depth = NULL;
    SMENU* cache[WIKI_SIDE_SIDES];
    SCASTING* picks = NULL;
    WIKICASTING* casting = NULL;
    STYPE* types = NULL;
    memset(&S, 0, sizeof S);
    memset(cache, 0, sizeof cache);
    S.in = in;
    S.vfs = vfs;
    S.common = -1;
    *report = NULL;
    err[0] = 0;
    for (i = 0; i < WIKI_SIDE_SIDES; i++) side_map[i] = -1;
    sb_raw(&S.rep, "{\"format\":1,\"report\":[");
    if (!vfs) { snprintf(err, sizeof err, "the sources did not open"); goto done; }
    /* the palette, as gfx.py opens it: <palettes>\<palette>.PAL, 256 entries of 4 bytes */
    if (!tagpu_vfs_find_built(vfs, in->names.palettes, in->names.palette, "PAL", &f) || f.size < 1024 ||
        !tagpu_vfs_read(vfs, &f, 0, 1024, pal, why, sizeof why)) {
        snprintf(err, sizeof err, "no %s\\%s.PAL of 256 colours", in->names.palettes, in->names.palette);
        goto done;
    }
    for (i = 0; i < 256; i++) pal[4 * i + 3] = 255;
    units_index(&S);
    if (!sides_read(&S)) { snprintf(err, sizeof err, "no sides: sidedata.tdf is missing, refused or empty"); goto done; }
    tagpu_vfs_enum(vfs, in->names.download, "*.tdf", 0, dl_each, &S);
    if (S.failed) { snprintf(err, sizeof err, "out of memory"); goto done; }
    S.common = side_bank(&S, in->names.commongui);
    if (S.common < 0) side_rep(&S, "refused", "no %s\\%s.GAF", "commongui", in->names.anims, in->names.commongui);
    /* every side with MAIN2 and GEN screens is a layout side, in sidedata's order */
    for (i = 0; i < S.nside; i++) {
        char m2[SNAME + 8], gen[SNAME + 8];
        snprintf(m2, sizeof m2, "%sMAIN2", S.side[i].prefix);
        snprintf(gen, sizeof gen, "%sGEN", S.side[i].prefix);
        if (!screen_build(&S, m2, m2, -1, 0, NULL, 0) || !screen_build(&S, gen, gen, -1, 0, NULL, 0)) {
            side_rep(&S, "refused", "no MAIN2 and GEN screens", S.side[i].name);
            continue;
        }
        kept[nlay_side] = i;
        side_map[i] = nlay_side++;
    }
    if (!nlay_side) { snprintf(err, sizeof err, "no side has MAIN2 and GEN screens"); goto done; }

    /* THE CASTING of every layout side. BOUND: the walk and the depths are one slot a unit; the
       menus one SMENU a unit and a distinct prefix, shared by the sides that spell it alike */
    if (!(mine = (int*)malloc((size_t)(S.ndl ? S.ndl : 1) * sizeof *mine)) ||
        !(walk = (int*)malloc((size_t)(S.nunit ? S.nunit : 1) * sizeof *walk)) ||
        !(depth = (int*)malloc((size_t)(S.nunit ? S.nunit : 1) * sizeof *depth)) ||
        !(picks = (SCASTING*)calloc((size_t)nlay_side, sizeof *picks)) ||
        !(casting = (WIKICASTING*)calloc((size_t)nlay_side, sizeof *casting)) ||
        !(types = (STYPE*)calloc(WIKI_SIDE_TYPES, sizeof *types))) {
        snprintf(err, sizeof err, "out of memory");
        goto done;
    }
    for (k = 0; k < nlay_side && !S.failed; k++) {
        const SSIDE* sd = &S.side[kept[k]];
        SWALK W;
        int c, d, j;
        for (j = 0; j < k && tagpu_hpi_stricmp(S.side[kept[j]].prefix, sd->prefix, (size_t)-1); j++) {}
        if (j < k) cache[k] = cache[j];
        else if (!(cache[k] = (SMENU*)calloc((size_t)(S.nunit ? S.nunit : 1), sizeof *cache[k]))) { S.failed = 1; break; }
        W.S = &S;
        W.cache = cache[k];
        W.prefix = sd->prefix;
        W.mine = mine;
        side_casting(&W, sd->commander, walk, depth, &picks[k]);
        for (c = 0; c < WIKI_NCAST; c++) {
            uint16_t builds = 0;
            const int t = picks[k].pick[c];
            if (t < 0) continue;
            for (d = 0; d < WIKI_NCAST; d++)
                if (picks[k].pick[d] >= 0 && menu_has(menu(&W, t), picks[k].pick[d])) builds |= (uint16_t)(1u << d);
            cast_fill(&S.unit[t], builds, &casting[k].cast[c]);
        }
    }
    if (S.failed) { snprintf(err, sizeof err, "out of memory"); goto done; }

    /* EACH SIDE'S TYPES: its bound casts' types that have a page, read with that side's prefix (the
       <prefix>DL page follows the player's side, 0x41AD6C, and the tutorial plays the player's) */
    for (k = 0; k < nlay_side && !S.failed; k++) {
        const SSIDE* sd = &S.side[kept[k]];
        STYPE want[WIKI_NCAST];
        int nwant = 0, c, j, w;
        for (c = 0; c < WIKI_NCAST; c++) {
            const WIKICASTDEF* kc = &casting[k].cast[c];
            const SUNIT* u;
            char el[WIKI_ID_MAX + SNAME + 16];
            int nmine;
            if (!kc->bound) continue;
            for (j = 0; j < nwant && strcmp(want[j].name, kc->type); j++) {}
            if (j < nwant) continue;
            u = &S.unit[picks[k].pick[c]];
            memset(&want[nwant], 0, sizeof want[nwant]);
            nmine = dl_mine(&S, u, mine);
            if ((want[nwant].last = type_pages(&S, u->name, mine, nmine, &want[nwant].zero)) < 0) {
                snprintf(el, sizeof el, "type %s (%s)", kc->type, sd->name);
                side_rep(&S, "note", "no page: the router shows GEN for it", el);
                continue;
            }
            scopy(want[nwant].name, kc->type, sizeof want[nwant].name);
            want[nwant].side = k;
            type_files(&S, u, sd->prefix, &want[nwant]);
            nwant++;
        }
        /* WIKI_SIDE_TYPES: the layout's type table over every side; a side whose types would pass
           it keeps none */
        if (ntype + nwant > WIKI_SIDE_TYPES) {
            char el[SNAME + 16];
            snprintf(el, sizeof el, "side %s types", sd->name);
            side_rep(&S, "refused", "its %d types pass %d over all sides", el, nwant, WIKI_SIDE_TYPES);
            continue;
        }
        for (w = 0; w < nwant && !S.failed; w++) {
            STYPE* t = &want[w];
            const SUNIT* u = unit_get(&S, t->name);
            char el[WIKI_SIDE_NAME + SNAME + 16];
            int made = -1, p, ok = 1, nmade = 0, made_at[WIKI_SIDE_PAGES + 1], nmine = u ? dl_mine(&S, u, mine) : 0;
            snprintf(el, sizeof el, "type %s (%s)", t->name, sd->name);
            for (j = 0; j < ntype && made < 0; j++) if (!strcmp(types[j].name, t->name)) made = j;
            if (made >= 0 && same_files(&types[made], t)) {
                /* the same files as another side's: the same screens */
                types[ntype] = types[made];
                types[ntype++].side = k;
                continue;
            }
            if (made >= 0) snprintf(t->tag, sizeof t->tag, "@%d", k);
            for (p = t->zero ? 0 : 1; p <= t->last && ok; p++) {
                char name[WIKI_SIDE_NAME + 16];
                if (snprintf(name, sizeof name, "%s%d%s", t->name, p, t->tag) >= WIKI_SIDE_NAME) ok = 0;
            }
            if (!ok) { side_rep(&S, "refused", "a page's screen name passes 31 bytes", el); continue; }
            /* WIKI_SIDE_SCREENS: the layout's screen table; a type that would pass it is left out */
            if (live_screens(&S) + t->last + t->zero > WIKI_SIDE_SCREENS) {
                side_rep(&S, "refused", "its %d pages pass %d screens", el, t->last + t->zero, WIKI_SIDE_SCREENS);
                continue;
            }
            for (p = t->zero ? 0 : 1; p <= t->last && ok; p++) {
                char name[WIKI_SIDE_NAME + 16];
                snprintf(name, sizeof name, "%s%d%s", t->name, p, t->tag);
                if (screen_build(&S, name, t->file[p], p, t->last == 0, mine, nmine)) made_at[nmade++] = screen_at(&S, name);
                else ok = 0;
            }
            if (!ok) {
                for (j = 0; j < nmade; j++) if (made_at[j] >= 0) S.scr[made_at[j]].live = 0;
                side_rep(&S, "refused", "a page is refused", el);
                continue;
            }
            types[ntype++] = *t;
        }
        casting_notes(&S, sd, &casting[k], types, ntype, k);
    }
    if (S.failed) { snprintf(err, sizeof err, "out of memory"); goto done; }

    /* THE LAYOUT, then its one check */
    if (!(lay = (WIKILAYOUT*)calloc(1, sizeof *lay))) { snprintf(err, sizeof err, "out of memory"); goto done; }
    {
        int remap_n = 0, *remap = (int*)malloc((size_t)(S.nscr ? S.nscr : 1) * sizeof *remap);
        if (!remap) { snprintf(err, sizeof err, "out of memory"); goto done; }
        lay->format = 1;
        lay->width = WIKI_SIDE_W;
        lay->height = WIKI_SIDE_MIN_H;
        for (i = 0; i < S.nscr; i++) {
            remap[i] = -1;
            if (!S.scr[i].live) continue;
            if (remap_n == WIKI_SIDE_SCREENS) { snprintf(err, sizeof err, "more than %d screens", WIKI_SIDE_SCREENS); free(remap); goto done; }
            {
                WIKISCREEN* w = &lay->screen[remap_n];
                scopy(w->name, S.scr[i].name, sizeof w->name);
                w->y = S.scr[i].y; w->w = S.scr[i].w; w->h = S.scr[i].h;
                w->ngadget = S.scr[i].ng;
                memcpy(w->gadget, S.scr[i].g, (size_t)S.scr[i].ng * sizeof *w->gadget);
                if (w->y + w->h > lay->height) lay->height = w->y + w->h;
            }
            remap[i] = remap_n++;
        }
        lay->nscreen = remap_n;
        for (k = 0; k < nlay_side; k++) {
            const SSIDE* sd = &S.side[kept[k]];
            char m2[SNAME + 8], gen[SNAME + 8];
            snprintf(m2, sizeof m2, "%sMAIN2", sd->prefix);
            snprintf(gen, sizeof gen, "%sGEN", sd->prefix);
            scopy(lay->side[k].name, sd->name, WIKI_SIDE_NAME);
            scopy(lay->side[k].prefix, sd->prefix, WIKI_SIDE_NAME);
            lay->side[k].main2 = screen_at(&S, m2) >= 0 ? remap[screen_at(&S, m2)] : -1;
            lay->side[k].gen = screen_at(&S, gen) >= 0 ? remap[screen_at(&S, gen)] : -1;
        }
        lay->nside = nlay_side;
        lay->ntype = ntype;
        for (k = 0; k < ntype; k++) {
            WIKIPAGES* t = &lay->type[k];
            int p;
            char name[WIKI_SIDE_NAME + 16];
            scopy(t->name, types[k].name, sizeof t->name);
            t->side = types[k].side;
            t->npage = types[k].last;
            for (p = 1; p <= types[k].last; p++) {
                snprintf(name, sizeof name, "%s%d%s", types[k].name, p, types[k].tag);
                t->page[p - 1] = screen_at(&S, name) >= 0 ? remap[screen_at(&S, name)] : -1;
            }
            t->orders = -1;
            if (types[k].zero) {
                snprintf(name, sizeof name, "%s0%s", types[k].name, types[k].tag);
                t->orders = screen_at(&S, name) >= 0 ? remap[screen_at(&S, name)] : -1;
            }
        }
        free(remap);
    }
    if (!tagpu_wikilayout_check(lay, why, sizeof why)) { snprintf(err, sizeof err, "the layout is refused: %s", why); goto done; }

    /* PANELSIDE of every layout side, then the dots: registered after the screens, as the pack
       registers them */
    {
        SSEQ q;
        int panelside[WIKI_SIDE_SIDES], fx, nrad = 0, high = -1, rad[WIKISIDE_DOTS], dot_key = 0, have_q = 0;
        SSEQ qr, qh;
        for (k = 0; k < nlay_side; k++) {
            const SSIDE* sd = &S.side[kept[k]];
            const int bk = sd->intgaf[0] ? side_bank(&S, sd->intgaf) : -1;
            char el[SNAME + 16];
            panelside[k] = -1;
            if (side_seq(&S, bk, "PANELSIDE", &q)) {
                panelside[k] = q.f[0];
                free(q.f);
            } else {
                snprintf(el, sizeof el, "panelside %s", sd->name);
                side_rep(&S, "refused", "no PANELSIDE in %s\\%s.GAF", el, in->names.anims, sd->intgaf);
            }
        }
        fx = side_bank(&S, "fx");
        if (side_seq(&S, fx, "radlogo", &qr)) {
            have_q = 1;
            for (k = 0; k < qr.n && nrad < WIKISIDE_DOTS; k++) rad[nrad++] = qr.f[k];
            dot_key = S.fr[qr.f[0]].tkey;
            free(qr.f);
        }
        if (side_seq(&S, fx, "radlogohigh", &qh)) {
            high = qh.f[0];
            if (!have_q) dot_key = S.fr[qh.f[0]].tkey;
            free(qh.f);
        }
        if (S.failed) { snprintf(err, sizeof err, "out of memory"); goto done; }

        /* the tables */
        pal_sorted(pal, sum, idx);
        if (side_find(&S, in->names.palettes, in->names.guipal, "PAL", &f) && f.size >= 1024) {
            unsigned char g[1024];
            if (tagpu_vfs_read(vfs, &f, 0, 1024, g, why, sizeof why)) {
                /* 0x4AC7D0: guipal's colours matched into the palette, each to the first entry of
                   least L1 distance */
                for (i = 0; i < 256; i++) {
                    long best = 0x98967F;
                    int hit = 0, j;
                    for (j = 0; j < 256; j++) {
                        const long d = labs((long)g[4 * i] - pal[4 * j]) + labs((long)g[4 * i + 1] - pal[4 * j + 1]) +
                                       labs((long)g[4 * i + 2] - pal[4 * j + 2]);
                        if (d < best) { best = d; hit = j; }
                    }
                    gui[i] = (unsigned char)hit;
                }
                has_gui = 1;
                snprintf(gui_from, sizeof gui_from, "%s", f.path);
            }
        }
        if (!has_gui) side_rep(&S, "refused", "no %s\\%s.PAL", "minimap", in->names.palettes, in->names.guipal);
        if (!(alp = (unsigned char*)malloc(65536))) { snprintf(err, sizeof err, "out of memory"); goto done; }
        alp_table(&S, pal, sum, idx, alp, alp_from, sizeof alp_from);
        /* 0x4BAD30: each entry to the nearest entry to its grey, (R+G+B)/3 floored */
        for (i = 0; i < 256; i++) {
            const int v = (pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2]) / 3;
            seen_t[i] = (unsigned char)pal_nearest(pal, sum, idx, v, v, v);
        }
        /* GUI_ButtonDraw's tint over a greyed button (0x4A699B..0x4A69BA): the grey remap
           0x4BFE10, then the box shader 0x4BF4D0 at level -20, which reads <palette>.SHD (32 rows of
           256) at row 12 with a SIGNED index (movsx at 0x4BF5E8), so an index of 0x80..0xFF reads
           the row before; two remaps of a pixel are one table. Without the file the engine generates
           the table (0x4BADF0), which is not reproduced: greyed buttons are drawn untinted. */
        if (side_find(&S, in->names.palettes, in->names.palette, "SHD", &f) && f.size >= 32 * 256 &&
            (shd = (unsigned char*)malloc(32 * 256)) && tagpu_vfs_read(vfs, &f, 0, 32 * 256, shd, why, sizeof why)) {
            for (i = 0; i < 256; i++) greyed[i] = shd[(GREY_ROW - (seen_t[i] >= 128)) * 256 + seen_t[i]];
            has_greyed = 1;
            snprintf(shd_from, sizeof shd_from, "%s", f.path);
        } else side_rep(&S, "refused", "no %s\\%s.SHD of 32 rows: greyed buttons are drawn untinted", "greyed tint",
                        in->names.palettes, in->names.palette);

        /* THE SHEET: every frame placed in shelves 1024 wide, tallest first */
        if (!(order = (SFRAME**)malloc((size_t)(S.nfr ? S.nfr : 1) * sizeof *order))) { snprintf(err, sizeof err, "out of memory"); goto done; }
        for (i = 0; i < S.nfr; i++) order[i] = &S.fr[i];
        qsort(order, (size_t)S.nfr, sizeof *order, frame_taller);
        {
            int x = 0, y = 0, shelf = 0;
            for (i = 0; i < S.nfr; i++) {
                SFRAME* fr = order[i];
                if (x + fr->w > WIKISIDE_SHEET_W) { x = 0; y += shelf; shelf = 0; }
                fr->x = x;
                fr->y = y;
                x += fr->w;
                if (fr->h > shelf) shelf = fr->h;
            }
            sh = y + shelf > 1 ? y + shelf : 1;
        }
        if (sh > WIKISIDE_SHEET_MAXH) { snprintf(err, sizeof err, "a sheet of %d rows, past %d", sh, WIKISIDE_SHEET_MAXH); goto done; }
        if (!(sheet = (unsigned char*)calloc((size_t)WIKISIDE_SHEET_W * (size_t)sh, 1))) { snprintf(err, sizeof err, "out of memory"); goto done; }
        for (i = 0; i < S.nfr; i++) {
            const SFRAME* fr = &S.fr[i];
            int r;
            for (r = 0; r < fr->h; r++)
                memcpy(sheet + (size_t)(fr->y + r) * WIKISIDE_SHEET_W + fr->x, fr->px + (size_t)r * fr->w, (size_t)fr->w);
        }

        /* THE BLOCK: written whole here, and never again */
        {
            const size_t head = (sizeof(WIKISIDE) + 15) & ~(size_t)15, sheet_n = (size_t)WIKISIDE_SHEET_W * (size_t)sh;
            unsigned char* blk;
            WIKISIDE* w;
            size_t rep_at;
            int si, j, n;
            /* the report first, so the block can carry it */
            sb_raw(&S.rep, "],\"refused\":null,\"folders\":{\"gui\":");
            sb_q(&S.rep, in->names.gui, 0);
            sb_raw(&S.rep, ",\"unitpics\":");
            sb_q(&S.rep, in->names.unitpics, 0);
            sb_raw(&S.rep, ",\"gamedata\":");
            sb_q(&S.rep, in->names.gamedata, 0);
            sb_raw(&S.rep, ",\"download\":");
            sb_q(&S.rep, in->names.download, 0);
            sb_raw(&S.rep, ",\"anims\":");
            sb_q(&S.rep, in->names.anims, 0);
            sb_raw(&S.rep, ",\"commongui\":");
            sb_q(&S.rep, in->names.commongui, 0);
            sb_raw(&S.rep, ",\"units\":");
            sb_q(&S.rep, in->names.units, 0);
            sb_raw(&S.rep, "},\"layout\":{\"format\":1,\"width\":%d,\"height\":%d,\"sides\":[", lay->width, lay->height);
            for (i = 0; i < lay->nside; i++) {
                sb_raw(&S.rep, "%s{\"name\":", i ? "," : "");
                sb_q(&S.rep, lay->side[i].name, 0);
                sb_raw(&S.rep, ",\"prefix\":");
                sb_q(&S.rep, lay->side[i].prefix, 0);
                sb_raw(&S.rep, ",\"main2\":");
                sb_q(&S.rep, lay->screen[lay->side[i].main2].name, 0);
                sb_raw(&S.rep, ",\"gen\":");
                sb_q(&S.rep, lay->screen[lay->side[i].gen].name, 0);
                sb_raw(&S.rep, ",\"types\":{");
                for (j = 0, n = 0; j < lay->ntype; j++) {
                    const WIKIPAGES* t = &lay->type[j];
                    if (t->side != i) continue;
                    sb_raw(&S.rep, "%s", n++ ? "," : "");
                    sb_q(&S.rep, t->name, 0);
                    sb_raw(&S.rep, ":{\"pages\":[");
                    for (k = 0; k < t->npage; k++) {
                        sb_raw(&S.rep, "%s", k ? "," : "");
                        sb_q(&S.rep, lay->screen[t->page[k]].name, 0);
                    }
                    sb_raw(&S.rep, "],\"orders\":");
                    sb_q(&S.rep, t->orders >= 0 ? lay->screen[t->orders].name : NULL, 0);
                    sb_raw(&S.rep, "}");
                }
                sb_raw(&S.rep, "}}");
            }
            sb_raw(&S.rep, "],\"screens\":{");
            for (i = 0; i < lay->nscreen; i++) {
                const WIKISCREEN* w2 = &lay->screen[i];
                sb_raw(&S.rep, "%s", i ? "," : "");
                sb_q(&S.rep, w2->name, 0);
                sb_raw(&S.rep, ":{\"y\":%d,\"w\":%d,\"h\":%d,\"gadgets\":[", w2->y, w2->w, w2->h);
                for (k = 0; k < w2->ngadget; k++) {
                    const WIKIGADGET* g = &w2->gadget[k];
                    sb_raw(&S.rep, "%s{\"name\":", k ? "," : "");
                    sb_q(&S.rep, g->name, 0);
                    sb_raw(&S.rep, ",\"role\":\"%s\",\"rect\":[%d,%d,%d,%d]", ROLE_NAME[g->role], g->x, g->y, g->w, g->h);
                    if (g->role == WIKI_ROLE_BUILD) {
                        sb_raw(&S.rep, ",\"unit\":");
                        sb_q(&S.rep, g->unit[0] ? g->unit : NULL, 0);
                    }
                    sb_raw(&S.rep, ",\"grey\":%s%s}", g->grey ? "true" : "false", g->hidden ? ",\"hidden\":true" : "");
                }
                sb_raw(&S.rep, "]}");
            }
            sb_raw(&S.rep, "}},\"casting\":[");
            for (k = 0; k < nlay_side; k++) {
                sb_raw(&S.rep, "%s", k ? "," : "");
                casting_json(&S.rep, &S, &S.side[kept[k]], &picks[k], &casting[k]);
            }
            sb_raw(&S.rep, "],\"art\":{\"sheet\":{\"w\":%d,\"h\":%d,\"fnv\":\"%08lx\"},\"pal\":\"%08lx\",\"panelside\":[",
                   WIKISIDE_SHEET_W, sh, (unsigned long)fnv(FNV0, sheet, (size_t)WIKISIDE_SHEET_W * (size_t)sh),
                   (unsigned long)fnv(FNV0, pal, sizeof pal));
            for (k = 0; k < nlay_side; k++) {
                const int ps = panelside[k];
                sb_raw(&S.rep, "%s", k ? "," : "");
                if (ps >= 0) {
                    sb_raw(&S.rep, "{\"frame\":");
                    frame_json(&S.rep, &S.fr[ps], S.fr[ps].hx, S.fr[ps].hy);
                    sb_raw(&S.rep, ",\"key\":%d}", S.fr[ps].tkey);
                } else sb_raw(&S.rep, "null");
            }
            sb_raw(&S.rep, "]");
            sb_raw(&S.rep, ",\"screens\":{");
            for (si = 0, i = 0; i < S.nscr; i++) {
                const SSCREEN* c = &S.scr[i];
                if (!c->live) continue;
                sb_raw(&S.rep, "%s", si++ ? "," : "");
                sb_q(&S.rep, c->name, 0);
                sb_raw(&S.rep, ":{\"file\":");
                sb_q(&S.rep, c->file, 0);
                sb_raw(&S.rep, ",\"path\":");
                sb_q(&S.rep, c->path, 1);
                sb_raw(&S.rep, ",\"where\":");
                sb_q(&S.rep, tagpu_vfs_source(vfs, c->src), 1);
                sb_raw(&S.rep, ",\"panel\":");
                if (c->panel >= 0) {
                    sb_raw(&S.rep, "{\"frame\":");
                    frame_json(&S.rep, &S.fr[c->panel], c->phx, c->phy);
                    sb_raw(&S.rep, ",\"key\":%d,\"from\":", c->pkey);
                    sb_q(&S.rep, c->pfrom, 0);
                    sb_raw(&S.rep, ",\"path\":");
                    sb_q(&S.rep, c->ppath, 1);
                    sb_raw(&S.rep, ",\"where\":");
                    sb_q(&S.rep, tagpu_vfs_source(vfs, c->psrc), 1);
                    sb_raw(&S.rep, "}");
                } else sb_raw(&S.rep, "null");
                sb_raw(&S.rep, ",\"pics\":[");
                for (k = 0; k < c->ng; k++) {
                    const SPIC* p = &c->pic[k];
                    int j;
                    sb_raw(&S.rep, "%s{\"from\":\"%s\"", k ? "," : "", FROM_NAME[p->from]);
                    if (p->from != WIKISIDE_FROM_LABEL) {
                        sb_raw(&S.rep, ",\"path\":");
                        sb_q(&S.rep, p->path, 1);
                        sb_raw(&S.rep, ",\"where\":");
                        sb_q(&S.rep, tagpu_vfs_source(vfs, p->src), 1);
                        sb_raw(&S.rep, ",\"seq\":");
                        sb_q(&S.rep, p->from == WIKISIDE_FROM_UNITPICS ? NULL : p->seq, 0);
                        sb_raw(&S.rep, ",\"frames\":[");
                        for (j = 0; j < p->nf; j++) {
                            sb_raw(&S.rep, "%s", j ? "," : "");
                            frame_json(&S.rep, &S.fr[p->f[j]], 0, 0);
                        }
                        sb_raw(&S.rep, "],\"key\":%d", p->key);
                    }
                    if (p->from == WIKISIDE_FROM_LABEL || p->from == WIKISIDE_FROM_FALLBACK) {
                        sb_raw(&S.rep, ",\"text\":");
                        sb_q(&S.rep, p->text, 0);
                    }
                    sb_raw(&S.rep, ",\"attribs\":%lld}", p->attribs);
                }
                sb_raw(&S.rep, "],\"labels\":[");
                for (k = 0; k < c->nlabel; k++) {
                    sb_raw(&S.rep, "%s{\"rect\":[%d,%d,%d,%d],\"text\":", k ? "," : "", c->label[k].x, c->label[k].y,
                           c->label[k].w, c->label[k].h);
                    sb_q(&S.rep, c->label[k].text, 0);
                    sb_raw(&S.rep, "}");
                }
                sb_raw(&S.rep, "]}");
            }
            sb_raw(&S.rep, "},\"dots\":{\"radlogo\":");
            if (nrad) {
                sb_raw(&S.rep, "[");
                for (k = 0; k < nrad; k++) {
                    sb_raw(&S.rep, "%s", k ? "," : "");
                    frame_json(&S.rep, &S.fr[rad[k]], S.fr[rad[k]].hx, S.fr[rad[k]].hy);
                }
                sb_raw(&S.rep, "]");
            } else sb_raw(&S.rep, "null");
            sb_raw(&S.rep, ",\"high\":");
            if (high >= 0) frame_json(&S.rep, &S.fr[high], S.fr[high].hx, S.fr[high].hy); else sb_raw(&S.rep, "null");
            sb_raw(&S.rep, ",\"key\":%d},\"minimap\":{\"unexplored\":", dot_key);
            if (has_gui) sb_raw(&S.rep, "%d,\"box\":%d", gui[0x00], gui[0x0E]); else sb_raw(&S.rep, "null,\"box\":null");
            sb_raw(&S.rep, ",\"gui\":");
            sb_q(&S.rep, has_gui ? gui_from : NULL, 1);
            sb_raw(&S.rep, ",\"seen\":\"%08lx\",\"alp\":{\"fnv\":\"%08lx\",\"path\":", (unsigned long)fnv(FNV0, seen_t, 256),
                   (unsigned long)fnv(FNV0, alp, 65536));
            sb_q(&S.rep, alp_from[0] ? alp_from : NULL, 1);
            sb_raw(&S.rep, "}},\"greyed\":");
            if (has_greyed) sb_raw(&S.rep, "\"%08lx\"", (unsigned long)fnv(FNV0, greyed, 256)); else sb_raw(&S.rep, "null");
            sb_raw(&S.rep, "}}\n");
            closed = 1;
            if (S.rep.oom || !S.rep.s) { snprintf(err, sizeof err, "out of memory for the report"); goto done; }
            rep_at = head + sheet_n;
            if (!(blk = (unsigned char*)calloc(1, rep_at + S.rep.n + 1))) { snprintf(err, sizeof err, "out of memory"); goto done; }
            w = (WIKISIDE*)blk;
            memcpy(blk + head, sheet, sheet_n);
            w->layout = *lay;
            w->sheet = blk + head;
            w->sheet_w = WIKISIDE_SHEET_W;
            w->sheet_h = sh;
            memcpy(w->pal, pal, sizeof w->pal);
            for (k = 0; k < nlay_side; k++) {
                const int ps = panelside[k];
                if ((w->has_panelside[k] = ps >= 0) != 0) {
                    w->panelside[k] = sheet_rect(&S.fr[ps], S.fr[ps].hx, S.fr[ps].hy);
                    w->panelside_key[k] = S.fr[ps].tkey;
                }
            }
            w->nradlogo = nrad;
            for (k = 0; k < nrad; k++) w->radlogo[k] = sheet_rect(&S.fr[rad[k]], S.fr[rad[k]].hx, S.fr[rad[k]].hy);
            if ((w->has_high = high >= 0) != 0) w->high = sheet_rect(&S.fr[high], S.fr[high].hx, S.fr[high].hy);
            w->dot_key = dot_key;
            w->unexplored = has_gui ? gui[0x00] : -1;
            w->box = has_gui ? gui[0x0E] : -1;
            memcpy(w->seen, seen_t, 256);
            if ((w->has_greyed = has_greyed) != 0) memcpy(w->greyed, greyed, 256);
            memcpy(w->alp, alp, 65536);
            for (si = 0, i = 0; i < S.nscr; i++) {
                const SSCREEN* c = &S.scr[i];
                WIKISIDESCREEN* o;
                if (!c->live) continue;
                o = &w->screen[si++];
                if ((o->has_panel = c->panel >= 0) != 0) {
                    o->panel = sheet_rect(&S.fr[c->panel], c->phx, c->phy);
                    o->panel_key = c->pkey;
                }
                for (k = 0; k < c->ng; k++) {
                    const SPIC* p = &c->pic[k];
                    WIKISIDEPIC* q = &o->pic[k];
                    int j;
                    q->from = p->from;
                    q->nframe = p->nf;
                    for (j = 0; j < p->nf; j++) q->frame[j] = sheet_rect(&S.fr[p->f[j]], 0, 0);
                    q->key = p->key;
                    q->attribs = (int)(p->attribs > 0x7FFFFFFF ? 0x7FFFFFFF : p->attribs < -0x7FFFFFFF ? -0x7FFFFFFF : p->attribs);
                    memcpy(q->text, p->text, sizeof q->text);
                }
                o->nlabel = c->nlabel;
                memcpy(o->label, c->label, sizeof o->label);
            }
            memcpy(w->casting, casting, (size_t)nlay_side * sizeof *casting);
            for (i = 0; i < WIKI_SIDE_SIDES; i++) w->side_of_engine[i] = side_map[i];
            memcpy(blk + rep_at, S.rep.s, S.rep.n + 1);
            w->report = (const char*)blk + rep_at;
            /* THE BOUND every composer read rests on: each frame rect inside the sheet */
            if (!side_frames_inside(w)) {
                snprintf(err, sizeof err, "a frame outside the sheet");
                free(blk);
                goto done;
            }
            out = w;
            refused = 0;
        }
    }
done:
    if (refused) {
        LOG("wikiside: the sidebar is refused - %s", err[0] ? err : "?");
        if (closed) {
            /* the report was finished for a block that is not made: it says only the refusal */
            free(S.rep.s);
            memset(&S.rep, 0, sizeof S.rep);
            sb_raw(&S.rep, "{\"format\":1,\"report\":[");
        }
        sb_raw(&S.rep, "],\"refused\":");
        sb_q(&S.rep, err[0] ? err : "?", 0);
        sb_raw(&S.rep, "}\n");
    }
    if (!S.rep.oom && S.rep.s) {
        *report = S.rep.s;
        S.rep.s = NULL;
    }
    free(S.rep.s);
    free(S.unit);
    free(S.dl);
    free(S.dlfile);
    for (i = 0; i < S.nbank; i++) bank_free(&S.bank[i]);
    free(S.bank);
    for (i = 0; i < S.nfr; i++) { free(S.fr[i].key); free(S.fr[i].px); }
    free(S.fr);
    free(S.scr);
    for (k = 0; k < WIKI_SIDE_SIDES; k++) {
        int j;
        if (!cache[k]) continue;
        for (j = k + 1; j < WIKI_SIDE_SIDES; j++) if (cache[j] == cache[k]) cache[j] = NULL;
        for (j = 0; j < S.nunit; j++) free(cache[k][j].u);
        free(cache[k]);
    }
    free(mine);
    free(walk);
    free(depth);
    free(picks);
    free(casting);
    free(types);
    free(lay);
    free(alp);
    free(shd);
    free(order);
    free(sheet);
    return out;
}

int tagpu_wikiside_minimap(const WIKI3DSTAGE* st, const unsigned char* alp, unsigned char* out,
                           int* w, int* h, int* offx, int* offy)
{
    int W, H, mw, mh, cols, sw, shh, x, y, n;
    unsigned char* src;
    if (!st || !st->ok || !st->tiles || !st->tilemap || !alp || st->w < 2 || st->h < 2 || st->w & 1 || st->h & 1 ||
        st->w > WIKI_MAP_MAX || st->h > WIKI_MAP_MAX || st->ntile < 1)
        return 0;
    W = st->w * 16;
    H = st->h * 16;
    if (W >= H) { mw = 126; mh = 126 * H / W; *offx = 0; *offy = (126 - mh) / 2; }
    else { mw = 126 * W / H; mh = 126; *offx = (126 - mw) / 2; *offy = 0; }
    if (mw < 1 || mh < 1) return 0;
    cols = st->w / 2;
    sw = 2 * mw;
    shh = 2 * mh;
    n = st->ntile;
    if (!(src = (unsigned char*)malloc((size_t)(sw * shh)))) return 0;
    /* BOUND: wy < H and wx < W, so (wy >> 5, wx >> 5) is a cell of the (w/2) x (h/2) tile map; an
       index past the tile set reads tile 0, as the engine's does */
    for (y = 0; y < shh; y++) {
        const int wy = H * y / shh;
        for (x = 0; x < sw; x++) {
            const int wx = W * x / sw, t = st->tilemap[(wy >> 5) * cols + (wx >> 5)];
            src[y * sw + x] = st->tiles[(size_t)(t < n ? t : 0) * TILE_BYTES + (size_t)((wy & 31) * 32 + (wx & 31))];
        }
    }
    for (y = 0; y < mh; y++) {
        const unsigned char *top = src + 2 * y * sw, *bot = src + (2 * y + 1) * sw;
        for (x = 0; x < mw; x++)
            out[y * mw + x] = alp[alp[top[2 * x] * 256 + top[2 * x + 1]] * 256 + alp[bot[2 * x] * 256 + bot[2 * x + 1]]];
    }
    free(src);
    *w = mw;
    *h = mh;
    return 1;
}

/* ---- the load ----------------------------------------------------------------------------------- */

/* `name` appended to the n names unless one of them is it; the new count */
static int add_name(char (*names)[WIKI_ID_MAX], int n, const char* name)
{
    int j;
    for (j = 0; j < n && strcmp(names[j], name); j++) {}
    if (j == n) scopy(names[n++], name, WIKI_ID_MAX);
    return n;
}

#define REPORT_RESERVE (1u << 20)   /* the arena's tail the report alone may use: assets that
                                       fill the rest are refused, and the report still fits      */

const WIKI3D* tagpu_wiki3d_load(const WIKI3DIN* in, TAGPU_VFS* vfs, const WIKISIDE* side, unsigned char* base,
                                uint32_t base32, uint32_t cap, uint32_t (*commit)(unsigned char* base, uint32_t upto))
{
    LOADER L;
    SB b;
    WIKI3D* w;
    STAGEREC* srec = NULL;
    UNITREC* urec = NULL;
    CURREC cur;
    uint32_t w_at, st_at = 0, un_at = 0, rep_at, assets;
    const WIKIDATA* d = in->data;
    char (*names)[WIKI_ID_MAX] = NULL;
    int i, k, s, n = 0, nunit = 0, nsides, nstage = d ? d->nentry : 0;
    memset(&L, 0, sizeof L);
    memset(&b, 0, sizeof b);
    memset(&cur, 0, sizeof cur);
    cur.frames[0] = cur.frames[1] = cur.frames[2] = -1;
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
    /* THE UNIT TYPES, gfx.py `stage_types` per entry, each name once over all: every entry's own,
       in the entries' order, each followed by every layout side's type for each cast the entry's
       units name, so a demo bound to any side's casting finds its models. BOUND: names holds the
       entries' types times one plus the sides, the most the loops below can add. */
    nsides = side ? side->layout.nside : 0;
    if (nsides < 0 || nsides > WIKI_SIDE_SIDES) nsides = 0;
    for (i = 0; i < nstage; i++) nunit += d->entry[i].ndef * (1 + nsides);
    if (nunit && !(names = (char (*)[WIKI_ID_MAX])calloc((size_t)nunit, sizeof *names))) nunit = 0;
    for (i = 0; names && i < nstage; i++) {
        const WIKIENTRY* e = &d->entry[i];
        for (k = 0; k < e->ndef; k++) n = add_name(names, n, e->def[k].name);
        for (s = 0; s < nsides; s++)
            for (k = 0; k < e->ndef; k++) {
                const int c = e->def[k].cast;
                if (c >= 0 && c < WIKI_NCAST && side->casting[s].cast[c].bound) n = add_name(names, n, side->casting[s].cast[c].type);
            }
    }
    nunit = names ? n : 0;
    if (nunit && (!(un_at = alloc(&L.a, (uint32_t)(nunit * sizeof(WIKI3DUNIT)))) ||
                  !(urec = (UNITREC*)calloc((size_t)nunit, sizeof *urec))))
        nunit = 0;
    L.vfs = vfs ? vfs : tagpu_vfs_open(in->cwd, (const char (*)[TAGPU_VFS_PATHMAX])in->arc, in->narc, in->names.language);
    if (L.vfs) {
        for (i = 0; i < nstage; i++) {
            WIKI3DSTAGE* st = (WIKI3DSTAGE*)AT(&L.a, st_at) + i;
            unsigned char mm[126 * 126];
            uint32_t at;
            stage_load(&L, &d->entry[i], st, &srec[i]);
            /* the sidebar's minimap of this stage, with the sidebar's shrink table */
            if (side && tagpu_wikiside_minimap(st, side->alp, mm, &st->mm_w, &st->mm_h, &st->mm_offx, &st->mm_offy) &&
                (at = alloc(&L.a, (uint32_t)(st->mm_w * st->mm_h))) != 0) {
                memcpy(AT(&L.a, at), mm, (size_t)(st->mm_w * st->mm_h));
                st->minimap = AT(&L.a, at);
            }
        }
        for (i = 0; i < nunit; i++) unit_load(&L, names[i], (WIKI3DUNIT*)AT(&L.a, un_at) + i, &urec[i]);
        /* every model is in: the bank is searched once for every name they hold, then bound */
        bank_all(&L);
        bind_all(&L);
        cursors_load(&L, &cur);
    } else
        snprintf(cur.why, sizeof cur.why, "the sources did not open");
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
    cursors_report(&b, &cur);
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
        for (k = 0; k < 3; k++) w->cursor[k] = cur.seq[k];
        w->base = base32;
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
    if (!vfs) tagpu_vfs_close(L.vfs);
    return w;
}

/* ---- the DLL's thread, publish and trigger --------------------------------------------------------- */

#ifdef _WIN32

#define ARENA_CAP   (32u << 20)     /* reserved whole; committed as the load grows into it       */
#define COMMIT_STEP (1u << 20)
#define DUMP_ASK    "tagpu_wiki3d.dump"
#define DUMP_OUT    "tagpu_wiki3d.json"
#define SIDE_OUT    "tagpu_wikiside.json"

static volatile LONG s_started;
/* THE LOAD'S ANSWER, once: NULL while the loader runs, then for good the WIKI3D or &s_3d_refused,
   by one interlocked store (tagpu_wiki3d_answer) */
static PVOID volatile s_pub;
static const char s_3d_refused = 0;
static void publish_3d(const WIKI3D* w)
{
    InterlockedExchangePointer((PVOID*)&s_pub, w ? (PVOID)w : (PVOID)&s_3d_refused);
}
/* THE SIDEBAR'S ANSWER, once: NULL while the loader decides, then for good the WIKISIDE or
   &s_side_refused, by one interlocked store (tagpu_wikiside_get) */
static PVOID volatile s_side;
static const char s_side_refused = 0;

/* ORDERING: the WIKISIDE behind `side` is written whole by tagpu_wikiside_load before this store,
   and InterlockedExchangePointer is a full barrier, so a reader that sees the pointer through
   tagpu_wikiside_get's interlocked read sees every byte of the block. Called once per process,
   before the 3D load starts. */
static void side_publish(const WIKISIDE* side)
{
    InterlockedExchangePointer((PVOID*)&s_side, side ? (PVOID)side : (PVOID)&s_side_refused);
}

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
    const WIKISIDE* side = NULL;
    TAGPU_VFS* vfs;
    char* side_report = NULL;
    LARGE_INTEGER f, t0, ts, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    /* THE SIDEBAR FIRST, published before the 3D load starts, refused or not: the wiki starts no
       demo before it has this answer (tagpu_wikiside_get) */
    vfs = tagpu_vfs_open(in->cwd, (const char (*)[TAGPU_VFS_PATHMAX])in->arc, in->narc, in->names.language);
    side = tagpu_wikiside_load(in, vfs, &side_report);
    free(side_report);                      /* the block keeps its own copy */
    QueryPerformanceCounter(&ts);
    if (side)
        LOG("wikiside: loaded in %.1f ms - %d screens, %d types, %d rows, a %d-row sheet",
            (double)(ts.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, side->layout.nscreen,
            side->layout.ntype, side->layout.height, side->sheet_h);
    side_publish(side);
    /* THE ARENA'S RANGE, checked once here: the block is refused whole unless both its ends pass the
       readers' filter, so every pointer in it does. It is never freed once published; one refused
       here was never seen by anyone. */
    if (!(base = (unsigned char*)VirtualAlloc(NULL, ARENA_CAP, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE))) {
        LOG("wiki3d: NOT loaded - %lu MB of address space could not be reserved (%lu)", (unsigned long)(ARENA_CAP >> 20),
            (unsigned long)GetLastError());
        tagpu_vfs_close(vfs);
        publish_3d(NULL);
        free(in);
        return 0;
    }
    if (!tagpu_m3_ptr_ok(base) || !tagpu_m3_ptr_ok(base + ARENA_CAP - 1)) {
        LOG("wiki3d: NOT loaded - the arena at %p lies outside the readers' range", (void*)base);
        VirtualFree(base, 0, MEM_RELEASE);
        tagpu_vfs_close(vfs);
        publish_3d(NULL);
        free(in);
        return 0;
    }
    w = tagpu_wiki3d_load(in, vfs, side, base, (uint32_t)(uintptr_t)base, ARENA_CAP, commit_pages);
    tagpu_vfs_close(vfs);
    QueryPerformanceCounter(&t1);
    if (!w) {
        LOG("wiki3d: NOT loaded - the arena could not hold the report");
        VirtualFree(base, 0, MEM_RELEASE);      /* never published, so no reader holds it */
        publish_3d(NULL);
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
    publish_3d(w);
    free(in);
    return 0;
}

void tagpu_wikiasset_start(WIKI3DIN* in)
{
    HANDLE t;
    if (InterlockedCompareExchange(&s_started, 1, 0) != 0) { free(in); return; }
    if (!(t = CreateThread(NULL, 0, loader, in, CREATE_SUSPENDED, NULL))) {
        LOG("wiki3d: NOT loaded - CreateThread failed (%lu)", (unsigned long)GetLastError());
        side_publish(NULL);
        publish_3d(NULL);
        free(in);
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(t);
    CloseHandle(t);
}

const WIKI3D* tagpu_wiki3d_get(void)
{
    const WIKI3D* w;
    return tagpu_wiki3d_answer(&w) == WIKISIDE_READY ? w : NULL;
}

/* ORDERING: an interlocked read, a full barrier, pairs with publish_3d's store; the block is never
   written after it */
int tagpu_wiki3d_answer(const WIKI3D** out)
{
    const PVOID v = InterlockedCompareExchangePointer((PVOID*)&s_pub, NULL, NULL);
    *out = NULL;
    if (!v) return WIKISIDE_PENDING;
    if (v == (PVOID)&s_3d_refused) return WIKISIDE_REFUSED;
    *out = (const WIKI3D*)v;
    return WIKISIDE_READY;
}

/* ORDERING: an interlocked read, a full barrier, pairs with side_publish's store; the block is
   never written after it (tagpu_wikiasset.h, THE SIDEBAR'S PUBLICATION) */
int tagpu_wikiside_get(const WIKISIDE** out)
{
    const PVOID v = InterlockedCompareExchangePointer((PVOID*)&s_side, NULL, NULL);
    *out = NULL;
    if (!v) return WIKISIDE_PENDING;
    if (v == (PVOID)&s_side_refused) return WIKISIDE_REFUSED;
    *out = (const WIKISIDE*)v;
    return WIKISIDE_READY;
}

/* `text` into `name` through a temporary file, so a reader never sees half of it */
static void dump_write(const char* name, const char* text)
{
    char tmp[64];
    FILE* f;
    snprintf(tmp, sizeof tmp, "%s.tmp", name);
    if (!(f = fopen(tmp, "wb"))) { LOG("wiki3d: the report could not open %s", tmp); return; }
    fputs(text, f);
    if (fclose(f) != 0 || !MoveFileExA(tmp, name, MOVEFILE_REPLACE_EXISTING)) {
        LOG("wiki3d: the report could not be written (%lu)", (unsigned long)GetLastError());
        return;
    }
    LOG("wiki3d: the report written to %s", name);
}

void tagpu_wikiasset_poll(void)
{
    const WIKI3D* w;
    const WIKISIDE* side;
    if (GetFileAttributesA(DUMP_ASK) == INVALID_FILE_ATTRIBUTES || !(w = tagpu_wiki3d_get())) return;
    DeleteFileA(DUMP_ASK);
    dump_write(DUMP_OUT, w->report);
    if (tagpu_wikiside_get(&side) == WIKISIDE_READY) dump_write(SIDE_OUT, side->report);
}

#endif
