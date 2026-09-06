/* tagpu_gaf.c — GAF frame decoding and the shared GL shelf atlas.

   Lifted out of tagpu_fx.c when the feature pass (G13a) needed the same two
   pieces; the effects, particle and feature passes now share one decoder and
   one atlas implementation, each with its own storage and lifetime.

   RLE rows (TA's own): a u16 byte length, then codes — `b&1` skips `b>>1`
   texels, `b&2` repeats the next byte `(b>>2)+1` times, otherwise `(b>>2)+1`
   literals follow. Uncompressed frames are w*h top-down bytes. Either way a
   texel IS a palette index: the atlas is GL_R8 sampled NEAREST, so the shader
   gets the index back exactly and does its own colour-key discard and palette
   lookup. Read-only over the engine. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_gaf.h"
#include "tagpu_restoreglsl.h"

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void glog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* one scratch plane for every atlas: decoding happens only inside
   tagpu_gaf_atlas_get, on the render thread, and the bytes are consumed by
   the upload before the call returns */
static unsigned char s_dec[TAGPU_GAF_DECMAX * TAGPU_GAF_DECMAX];
/* the frame re-emitted with a 1-texel replicated border on all four sides */
static unsigned char s_pad[(TAGPU_GAF_DECMAX + 2) * (TAGPU_GAF_DECMAX + 2)];

const unsigned char* tagpu_gaf_frame_sane(const void* g0)
{
    const unsigned char* g = (const unsigned char*)g0;
    int w, h;
    if (!ptr_ok(g) || IsBadReadPtr(g, 0x18)) return NULL;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX) return NULL;
    return g;
}

/* GAF_SequenceIndex2Frame 0x4B7F30: seq+0x28 entry table, stride 8 */
const unsigned char* tagpu_gaf_seq_frame(const char* seq, int idx)
{
    int n;
    const char* tab;
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return NULL;
    n = *(const unsigned short*)(seq + TAGPU_SQ_N);
    if (idx < 0 || idx >= n || n > 4096) return NULL;
    tab = seq + TAGPU_SQ_TAB;
    if (IsBadReadPtr(tab, (SIZE_T)(idx + 1) * 8)) return NULL;
    return tagpu_gaf_frame_sane(*(const void* const*)(tab + idx * 8));
}

int tagpu_gaf_seq_nframes(const char* seq)
{
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return 0;
    return *(const unsigned short*)(seq + TAGPU_SQ_N);
}

/* GAFGetCurrentFramePtrAddr 0x4B7EE0: {u16 frame @0; ...; seq* @8} — a pure
   read, it does not advance the frame (the sim tick does) */
const unsigned char* tagpu_gaf_state_frame(const char* st)
{
    const char* seq;
    if (!ptr_ok(st) || IsBadReadPtr(st, 0x0C)) return NULL;
    seq = *(const char* const*)(st + TAGPU_AS_SEQ);
    return tagpu_gaf_seq_frame(seq, *(const unsigned short*)(st + TAGPU_AS_FRAME));
}

const char* tagpu_gaf_seq_name(const char* seq)
{
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return "?";
    return seq + TAGPU_SQ_NAME;
}

int tagpu_gaf_decode(const unsigned char* g, int w, int h, unsigned char* out)
{
    unsigned char ck = g[TAGPU_GF_CK], comp = g[TAGPU_GF_COMP];
    const unsigned char* px = *(const unsigned char* const*)(g + TAGPU_GF_PIX);
    const unsigned char* p;
    int y;
    if (!ptr_ok(px)) return 0;
    if (comp == 0) {
        if (IsBadReadPtr(px, (SIZE_T)w * h)) return 0;
        for (y = 0; y < h; y++) memcpy(out + (size_t)y * w, px + (size_t)y * w, (size_t)w);
        return 1;
    }
    memset(out, ck, (size_t)w * h);
    p = px;
    for (y = 0; y < h; y++) {
        int rowlen, x = 0;
        const unsigned char* q;
        unsigned char* row = out + (size_t)y * w;
        if (IsBadReadPtr(p, 2)) return 0;
        rowlen = *(const unsigned short*)p; p += 2;
        if (rowlen > 8192 || IsBadReadPtr(p, rowlen)) return 0;
        q = p;
        while (q < p + rowlen && x < w) {
            unsigned char b = *q++;
            if (b & 1) x += b >> 1;
            else if (b & 2) {
                int n = (b >> 2) + 1, i;
                unsigned char v;
                if (q >= p + rowlen) break;
                v = *q++;
                for (i = 0; i < n && x < w; i++) row[x++] = v;
            } else {
                int n = (b >> 2) + 1, i;
                for (i = 0; i < n && q < p + rowlen; i++) {
                    unsigned char v = *q++;
                    if (x < w) row[x] = v;
                    x++;
                }
            }
        }
        p += rowlen;
    }
    return 1;
}

void tagpu_gaf_atlas_reset(TAGPU_GAFATLAS* a)
{
    char b[128];
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    memset(a->hash, 0, sizeof a->hash);
    /* the twin's rects are about to be re-used by other frames: back to
       unpainted, and whatever was queued is dropped (it re-queues on its miss) */
    if (a->job) tagpu_rglsl_job_clear(a->job);
    _snprintf(b, sizeof b, "%s: atlas reset (full) — frames re-decode on demand",
              a->tag ? a->tag : "gaf");
    glog(b);
}

void tagpu_gaf_atlas_lost(TAGPU_GAFATLAS* a)
{
    a->tex = 0; a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    memset(a->hash, 0, sizeof a->hash);
    /* the twin and the job died with the context (tagpu_rglsl_glreset has
       already forgotten the job: it runs first); re-armed on the next frame */
    a->rgb = 0; a->job = NULL; a->restoreFailed = 0;
}

/* one frame onto the restore queue: the R8 atlas is the source, the twin the
   destination, same rect, the 1-texel border painted as a copy of the edge */
static void restore_enqueue(TAGPU_GAFATLAS* a, const TAGPU_GAFENT* e)
{
    TAGPU_RGLSL_FRAME f;
    f.ax = f.dx = e->x; f.ay = f.dy = e->y;
    f.w = e->w; f.h = e->h; f.wrap = e->wrap; f.border = 1; f.key = e->ck;
    tagpu_rglsl_job_add(a->job, &f, 1);
}

/* tagpu_restoredump.on: the twin as the shader samples it, once per fill of
   the atlas, as raw bytes -- the restorer's only disk write, and only under
   the trigger (the terrain pass writes its own). Three files per atlas:
   tagpu_restore_<tag>.r8 (the source, dim x dim), .rgba (the twin, dim x dim
   x 4) and .idx (a line per entry: x y w h key wrap), so `tascene featdiff`
   can find each frame in both and hold the twin to the lab's bar. */
static void dump_if_armed(TAGPU_GAFATLAS* a)
{
    static unsigned s_check;
    char name[64], b[160];
    unsigned char* buf;
    FILE* f;
    int i;
    if (!a->job || a->n == 0 || a->n == a->dumpedN) return;
    if (!tagpu_rglsl_job_idle(a->job)) return;
    if (++s_check % 60) return;                    /* one attribute read a second */
    if (GetFileAttributesA("tagpu_restoredump.on") == INVALID_FILE_ATTRIBUTES) return;
    buf = (unsigned char*)malloc((size_t)a->dim * a->dim * 4);
    if (!buf) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    _snprintf(name, sizeof name, "tagpu_restore_%s.r8", a->tag);
    glBindTexture(GL_TEXTURE_2D, a->tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf);
    f = fopen(name, "wb");
    if (f) { fwrite(buf, 1, (size_t)a->dim * a->dim, f); fclose(f); }
    _snprintf(name, sizeof name, "tagpu_restore_%s.rgba", a->tag);
    glBindTexture(GL_TEXTURE_2D, a->rgb);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    f = fopen(name, "wb");
    if (f) { fwrite(buf, 1, (size_t)a->dim * a->dim * 4, f); fclose(f); }
    free(buf);
    _snprintf(name, sizeof name, "tagpu_restore_%s.idx", a->tag);
    f = fopen(name, "w");
    if (f) {
        for (i = 0; i < a->n; i++) if (a->ents[i].ok)
            fprintf(f, "%d %d %d %d %d %d\n", a->ents[i].x, a->ents[i].y, a->ents[i].w, a->ents[i].h,
                    a->ents[i].ck, a->ents[i].wrap);
        fclose(f);
    }
    a->dumpedN = a->n;
    _snprintf(b, sizeof b, "%s: restored twin dumped to tagpu_restore_%s.{r8,rgba,idx} (%dx%d, %d entries)%s",
              a->tag, a->tag, a->dim, a->dim, a->n, f ? "" : " -- WRITE FAILED");
    glog(b);
}

void tagpu_gaf_atlas_restore(TAGPU_GAFATLAS* a, const unsigned char* pal)
{
    int i;
    a->pal = pal;
    if (a->job) { dump_if_armed(a); return; }
    if (a->restoreFailed || !a->tex || !pal) return;
    if (!tagpu_classicpp_on()) return;
    if (!a->rgb) {
        GLuint t = 0;
        glGenTextures(1, &t);
        if (!t) { a->restoreFailed = 1; return; }
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, a->dim, a->dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        a->rgb = t;
    }
    a->job = tagpu_rglsl_job_new(a->tag ? a->tag : "gaf", a->prio, 0,
                                 a->tex, a->dim, a->dim, pal, a->rgb, a->dim, a->dim);
    if (!a->job) { a->restoreFailed = 1; return; }     /* the reason is in tagpu.log */
    /* what is already in the atlas was uploaded before the switch: queue it,
       in upload order, so nothing stays indexed for want of a miss */
    for (i = 0; i < a->n; i++) if (a->ents[i].ok) restore_enqueue(a, &a->ents[i]);
}

/* frame headers are heap pointers: mix the high bits down so the low-order
   allocator alignment does not cluster every key into one bucket */
static unsigned gaf_hash(const void* p)
{
    unsigned h = (unsigned)(size_t)p;
    h ^= h >> 15; h *= 0x2545F491u; h ^= h >> 13;
    return h & (TAGPU_GAF_HASH - 1);
}

int tagpu_gaf_atlas_create(TAGPU_GAFATLAS* a)
{
    GLuint t = 0;
    if (a->tex) return 1;
    if (a->dim <= 0 || a->max <= 0 || !a->ents) return 0;
    glGenTextures(1, &t);
    if (!t) return 0;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, a->dim, a->dim, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    a->tex = t;
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    memset(a->hash, 0, sizeof a->hash);
    return 1;
}

const TAGPU_GAFENT* tagpu_gaf_atlas_get(TAGPU_GAFATLAS* a, const unsigned char* g)
{
    int w, h, x, y, slot, i;
    const void* pix;
    TAGPU_GAFENT* e;
    if (!tagpu_gaf_atlas_create(a)) return NULL;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    /* tagpu_gaf_frame_sane already promises this of every caller's frame; the
       decode into s_dec and the guard-rail copy below both index off it */
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX) return NULL;
    pix = *(const void* const*)(g + TAGPU_GF_PIX);
    for (slot = (int)gaf_hash(g); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == g && c->pix == pix && c->w == w && c->h == h)
            return c->ok ? c : NULL;
    }
    /* every frame carries its OWN 1-texel border, so the shelf advances by
       w+2 / h+2 rather than sharing one gutter between two neighbours */
    if (a->full || h + 2 > a->dim) return NULL;
    if (a->n >= a->max) { a->full = 1; return NULL; }
    if (a->shelfX + w + 2 > a->dim) { a->shelfY += a->shelfH + 2; a->shelfX = 0; a->shelfH = 0; }
    if (a->shelfY + h + 2 > a->dim) { a->full = 1; return NULL; }
    e = &a->ents[a->n];
    e->frame = g; e->pix = pix;
    e->w = (unsigned short)w; e->h = (unsigned short)h; e->ok = 0;
    /* a frame whose pixels are momentarily unreadable must stay retryable:
       claiming the slot here would cache the failure for the atlas's whole
       life, and the feature atlas is meant to live as long as the map */
    if (!tagpu_gaf_decode(g, w, h, s_dec)) return NULL;
    a->hash[slot] = ++a->n;
    x = a->shelfX + 1; y = a->shelfY + 1;          /* inside the border */
    a->shelfX += w + 2;
    if (h > a->shelfH) a->shelfH = h;
    glBindTexture(GL_TEXTURE_2D, a->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Re-emit the frame with its outermost row and column repeated all round.
       The border is what any sampler that reaches past the frame must land on:
       under GL_NEAREST that is the fragment whose centre falls exactly on the
       quad's far edge (its u interpolates to exactly u1, and floor(u1*dim) is
       one texel past the frame) — left unwritten that texel is whatever
       glTexImage2D(NULL) leaves, i.e. index 0, a real palette entry (black)
       rather than the frame's colour key, which is the black hairline down the
       right of every tree at zoom 0.25. Under a filtered sampler it is every
       edge fragment, which is why the border is on all four sides and not just
       the two the shelf packer used to leave spare. */
    {
        const int pw = w + 2;
        for (i = 0; i < h; i++) {
            unsigned char* row = s_pad + (size_t)(i + 1) * pw + 1;
            memcpy(row, s_dec + (size_t)i * w, (size_t)w);
            row[-1] = row[0];
            row[w]  = row[w - 1];
        }
        memcpy(s_pad, s_pad + (size_t)pw, (size_t)pw);
        memcpy(s_pad + (size_t)(h + 1) * pw, s_pad + (size_t)h * pw, (size_t)pw);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x - 1, y - 1, pw, h + 2,
                        GL_RED, GL_UNSIGNED_BYTE, s_pad);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    e->u0 = (float)x / (float)a->dim;         e->v0 = (float)y / (float)a->dim;
    e->u1 = (float)(x + w) / (float)a->dim;   e->v1 = (float)(y + h) / (float)a->dim;
    e->x = (unsigned short)x; e->y = (unsigned short)y;
    e->ck = g[TAGPU_GF_CK];
    /* decided here, while the pixels are still in s_dec: the tileability the
       restorer wrap-pads by (a key on an edge says no) */
    e->wrap = a->pal ? (char)tagpu_rglsl_tileable(s_dec, w, h, a->pal, e->ck) : 0;
    e->ok = 1;
    if (a->job) restore_enqueue(a, e);
    return e;
}
