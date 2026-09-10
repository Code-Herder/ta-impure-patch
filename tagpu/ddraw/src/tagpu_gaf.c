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
#include "tagpu_pal.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void glog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* GL 3.0's mip generation and GL 1.1's float texture parameter are not in
   opengl_utils.h; fetched once, the way every tagpu module fetches what the
   fork does not export (wglGetProcAddress first, then opengl32 itself) */
typedef void (APIENTRY* PFN_GENERATEMIPMAP)(GLenum);
typedef void (APIENTRY* PFN_TEXPARAMETERF)(GLenum, GLenum, GLfloat);
static PFN_GENERATEMIPMAP x_glGenerateMipmap;
static PFN_TEXPARAMETERF  x_glTexParameterf;
static int s_glFetched;
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#define TWIN_ANISO 4.0f         /* the lab's default (tascene-view.html aniso) */

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = (void*)GetProcAddress(gl, n);
    }
    return p;
}

static void fetch_gl(void)
{
    if (s_glFetched) return;
    s_glFetched = 1;
    x_glGenerateMipmap = (PFN_GENERATEMIPMAP)getgl("glGenerateMipmap");
    x_glTexParameterf  = (PFN_TEXPARAMETERF)getgl("glTexParameterf");
}

/* one scratch plane for every atlas: decoding happens only inside
   tagpu_gaf_atlas_get, on the render thread, and the bytes are consumed by
   the upload before the call returns */
static unsigned char s_dec[TAGPU_GAF_DECMAX * TAGPU_GAF_DECMAX];
/* the frame re-emitted with its replicated border (up to PADMAX texels) on
   all four sides */
static unsigned char s_pad[(TAGPU_GAF_DECMAX + 2 * TAGPU_GAF_PADMAX) * (TAGPU_GAF_DECMAX + 2 * TAGPU_GAF_PADMAX)];

/* the cell's edge rounded up to the atlas's alignment */
static int cell_up(const TAGPU_GAFATLAS* a, int v)
{
    return (v + a->align - 1) / a->align * a->align;
}

/* Rebuild the twin's mip levels 1..mip from level 0 -- after every batch the
   restorer painted, after a recycle cleared level 0 (the restorer clears
   only that level: tagpu_restoreglsl.c clear_dest), and once when the twin
   is made, so it is never sampled incomplete (an incomplete texture reads
   as opaque black, which the shader would take for a restored texel). */
static void twin_mips(TAGPU_GAFATLAS* a)
{
    if (!a->mip || !a->rgb || !x_glGenerateMipmap) return;
    glBindTexture(GL_TEXTURE_2D, a->rgb);
    x_glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    a->mippedN = tagpu_rglsl_job_painted(a->job);
}

/* ORDER IS THE WHOLE FIX. A shelf packer wastes the difference between the
   cell that opened a shelf and every shorter cell that then sat on it, and
   the feature atlas is fed in map order -- a 320-tall tree, then a row of
   12-tall rocks under it. Fed tallest-first the same 229 frames span 58% of
   a 2048 square instead of 86%, which is the difference between fitting and
   latching `full`. This is a COUNTING SORT over the cell height: the bins
   below cover every height atlas_get and atlas_put admit (h <= DECMAX, plus
   two borders of at most PADMAX, rounded up to at most PADMAX of alignment),
   and s_ordNext threads the entries sharing a bin. No comparator, no qsort
   context, no allocation -- a repack runs on the render thread inside a
   frame. Render thread only, one atlas at a time: only tagpu_gaf_atlas_reset
   reaches it, and it never re-enters. */
#define ORD_MAX   4096          /* >= every atlas's `max` (feat and gui: 4096) */
#define ORD_HBINS (TAGPU_GAF_DECMAX + 4 * TAGPU_GAF_PADMAX + 1)
static int s_ordNext[ORD_MAX];
static int s_ordHead[ORD_HBINS];

static int atlas_repack(TAGPU_GAFATLAS* a);

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

/* Throw the atlas away: no entries, no shelves, every UV invalid. Shared by
   the recycle and by tagpu_gaf_atlas_forget so there is one drop, not two. */
static void atlas_drop(TAGPU_GAFATLAS* a, const char* why)
{
    char b[128];
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    a->gen++;                   /* every UV in the atlas has just moved */
    memset(a->hash, 0, sizeof a->hash);
    /* the twin's rects are about to be re-used by other frames: back to
       unpainted, and whatever was queued is dropped (it re-queues on its miss) */
    if (a->job) { tagpu_rglsl_job_clear(a->job); twin_mips(a); }
    _snprintf(b, sizeof b, "%s: atlas reset (%s) — frames re-decode on demand, generation %u",
              a->tag ? a->tag : "gaf", why, a->gen);
    glog(b);
}

void tagpu_gaf_atlas_reset(TAGPU_GAFATLAS* a)
{
    int wasFull = a->full;
    /* PART 2: re-lay what is here instead of throwing it away. Only for an
       atlas that asked (`repack`), only when it actually filled -- a restart
       that is not "full" is the UI atlas re-arming or a context change, and
       both want the entries gone -- and only while a re-lay can still gain
       something. atlas_repack owns the log line and leaves the atlas usable. */
    if (wasFull && a->repack) {
        if (a->repackWall) return;      /* held: nothing a rebuild can improve */
        if (atlas_repack(a)) return;
    }
    /* the sprite atlases reset when full; the UI atlas also on a re-arm or a
       GL context change (tagpu_gui_surf.c twins_reset), which is not "full" */
    atlas_drop(a, wasFull ? "full" : "restart");
}

/* The atlas no longer describes anything the caller wants: drop the entries
   AND the repack decisions taken over them. This is the ONLY way back from
   the wall, and the feature pass owes it one call when the map changes --
   `repack` deliberately pins what it holds, and what it holds is only ever
   right for one map. Without it a session that walled on one map would carry
   that map's frames into the next and refuse every frame of it for the rest
   of the session. It goes straight to the drop: a repack here would re-lay
   the very entries we have just been told are meaningless. */
void tagpu_gaf_atlas_forget(TAGPU_GAFATLAS* a)
{
    a->repackWall = 0;
    atlas_drop(a, "subject replaced");
}

void tagpu_gaf_atlas_lost(TAGPU_GAFATLAS* a)
{
    a->tex = 0; a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    a->gen++;                   /* ...and again: the texture itself is gone */
    memset(a->hash, 0, sizeof a->hash);
    /* the twin and the job died with the context (tagpu_rglsl_glreset has
       already forgotten the job: it runs first); re-armed on the next frame */
    a->rgb = 0; a->job = NULL; a->restoreFailed = 0; a->mippedN = 0;
    /* the entries went with the texture, so the wall the last fill hit says
       nothing about the next one */
    a->repackWall = 0;
}

/* one frame onto the restore queue: the R8 atlas is the source, the twin the
   destination, same rect, the border -- and the cell's alignment slack past
   it -- painted as a copy of the edge, as the R8 upload painted them */
static void restore_enqueue(TAGPU_GAFATLAS* a, const TAGPU_GAFENT* e)
{
    TAGPU_RGLSL_FRAME f;
    if (a->restoreMinEdge > 0 && (e->w < a->restoreMinEdge || e->h < a->restoreMinEdge)) return;
    f.ax = f.dx = e->x; f.ay = f.dy = e->y;
    f.w = e->w; f.h = e->h; f.wrap = e->wrap; f.border = a->pad; f.key = e->ck;
    f.padR = cell_up(a, e->w + 2 * a->pad) - (e->w + 2 * a->pad);
    f.padB = cell_up(a, e->h + 2 * a->pad) - (e->h + 2 * a->pad);
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
    if (a->job) {
        /* The palette moved under the twin — the Gamma slider, or `+gamma N`
           in chat: every texel in it was restored through the old one and is
           now the wrong brightness beside the engine's own pixels. Re-point
           the job and queue every entry again, WITHOUT clearing, so the atlas
           recolours cell by cell instead of vanishing for the length of the
           repaint. Gated on the job being idle, which bounds this to one
           repaint of this atlas in flight however often the palette moves. */
        if (pal && a->palSerial != tagpu_pal_serial() && tagpu_rglsl_job_idle(a->job)) {
            char b[128];
            a->palSerial = tagpu_pal_serial();
            tagpu_rglsl_job_repalette(a->job, pal);
            for (i = 0; i < a->n; i++) if (a->ents[i].ok) restore_enqueue(a, &a->ents[i]);
            _snprintf(b, sizeof b, "%s: palette changed (serial=%u): %d entries queued for repaint",
                      a->tag, a->palSerial, a->n);
            glog(b);
        }
        /* a mipped twin: its levels follow level 0 one frame behind the
           batch that painted it (the OUT draw is issued after this call,
           in tagpu_rglsl_step; the next frame's call sees the count move) */
        if (a->mip && tagpu_rglsl_job_painted(a->job) != a->mippedN) twin_mips(a);
        dump_if_armed(a);
        return;
    }
    if (a->restoreFailed || !a->tex || !pal) return;
    if (!tagpu_classicpp_assets()) return;
    fetch_gl();
    if (a->mip && (!x_glGenerateMipmap || !x_glTexParameterf)) {
        char b[128];
        _snprintf(b, sizeof b, "%s: no glGenerateMipmap/glTexParameterf: twin left unmipped, NEAREST", a->tag);
        glog(b);
        a->mip = 0;
    }
    if (!a->rgb) {
        GLuint t = 0;
        glGenTextures(1, &t);
        if (!t) { a->restoreFailed = 1; return; }
        glBindTexture(GL_TEXTURE_2D, t);
        if (a->mip) {
            /* renderers.md 1: trilinear to level `mip`, anisotropic where the
               extension answers -- a driver without it raises INVALID_ENUM
               on the parameter and is otherwise unaffected, so try it and
               read the error flag, drained first because it is process-wide */
            char b[160];
            int pending = 0;
            GLenum err;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, a->mip);
            while (glGetError() != GL_NO_ERROR && pending < 16) pending++;
            x_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, TWIN_ANISO);
            err = glGetError();
            _snprintf(b, sizeof b, "%s: restored twin %dx%d, trilinear to mip level %d, %s",
                      a->tag, a->dim, a->dim, a->mip,
                      err == GL_NO_ERROR ? "4x anisotropic" : "no anisotropic filtering (extension absent)");
            glog(b);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, a->dim, a->dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        a->rgb = t;
    }
    a->job = tagpu_rglsl_job_new(a->tag ? a->tag : "gaf", a->prio, 0,
                                 a->tex, a->dim, a->dim, pal, a->rgb, a->dim, a->dim);
    if (!a->job) {
        /* the reason is in tagpu.log. The twin goes with the job: the passes
           gate their restored branch on `rgb`, and a twin nothing has cleared
           is 16 MB of whatever the driver left there, alpha included */
        glDeleteTextures(1, &a->rgb);
        a->rgb = 0;
        a->restoreFailed = 1;
        return;
    }
    a->palSerial = tagpu_pal_serial();
    /* the job cleared level 0 to alpha 0: the mip levels must say the same
       before anything samples them */
    twin_mips(a);
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
    /* the layout parameters: unset is the sprite atlases' one-texel border */
    if (a->pad < 1) a->pad = 1;
    if (a->pad > TAGPU_GAF_PADMAX) a->pad = TAGPU_GAF_PADMAX;
    if (a->align < 1) a->align = 1;
    if (a->mip < 0) a->mip = 0;
    return 1;
}

/* THE REPACK. Re-lay every entry the atlas holds, tallest cell first, and
   leave each one RESERVED: the rect is assigned, nothing is uploaded, and
   the next atlas_get for that frame paints it in place (atlas_insert's probe
   below). We cannot move the texels ourselves -- an entry records the
   frame's address and its size, never the decoded bytes, and GL 3.3 core has
   no glCopyImageSubData to shuffle them with -- so the pixels come back the
   way they arrived the first time, by RLE decode on demand. That is the same
   work one of today's recycles does, done ONCE when the page fills instead
   of once per frame for as long as it stays full.

   It reads `w` and `h` out of an entry and nothing else: both were bounded to
   1..TAGPU_GAF_DECMAX by atlas_get/atlas_put before the entry existed, so the
   geometry below cannot address outside the atlas whatever the entry's `frame`
   pointer has since become. No pointer stored in an entry is dereferenced
   here. (A frame freed by the engine and re-allocated at the same address
   with the same `pix` and the same size would draw its old art -- that is the
   pre-existing hazard the (frame, pix, w, h) key already carries, and this
   widens the window it lives in: the feature atlas is meant to last as long
   as the map, but nothing tells it when the map changes. features.md 5.)

   Returns 1 when the atlas was re-laid (the caller must not go on to drop
   it), 0 when it was left exactly as it was found. */
static int atlas_repack(TAGPU_GAFATLAS* a)
{
    const int p = a->pad;
    const int before = a->n;
    char b[192];
    int i, hb, x = 0, y = 0, sh = 0, kept = 0, w;
    int wanted = 0, wanted_skip = 0;

    if (before <= 0 || before > ORD_MAX || !a->tex || !a->ents) return 0;

    /* Only what is still being ASKED FOR is re-laid. An entry nothing has
       touched since the last repack is not part of the set the atlas is short
       of room for -- most sharply after a map change, when every one of the
       previous map's frames is dead weight -- so it is dropped here and
       re-decodes on demand if it is ever wanted again. This is what keeps the
       atlas from accumulating across a session, and it is why the wall below
       can be read as "the LIVE set does not fit" rather than "the set we
       happen to be holding does not fit". */
    for (i = 0; i < ORD_HBINS; i++) s_ordHead[i] = -1;
    for (i = 0; i < before; i++) {
        const int ch = cell_up(a, (int)a->ents[i].h + 2 * p);
        if (ch < 1 || ch >= ORD_HBINS) return 0;   /* h was bounded; belt and braces */
        if (!a->ents[i].hit) { wanted_skip++; continue; }
        s_ordNext[i] = s_ordHead[ch];
        s_ordHead[ch] = i;
        wanted++;
    }
    /* Nothing was touched at all -- the caller filled the atlas inside a
       single frame without a lookup landing, which the hit marking makes
       impossible in practice. Re-lay everything rather than evict everything. */
    if (wanted == 0) {
        for (i = 0; i < ORD_HBINS; i++) s_ordHead[i] = -1;
        for (i = 0; i < before; i++) {
            const int ch = cell_up(a, (int)a->ents[i].h + 2 * p);
            s_ordNext[i] = s_ordHead[ch]; s_ordHead[ch] = i;
        }
        wanted = before; wanted_skip = 0;
    }

    /* Pass 1 -- the shelf geometry of atlas_insert, fed tallest bin first.
       Because the tallest cell on a shelf is always the one that opened it,
       `sh` is set once per shelf and never grown under a later cell, which is
       exactly the waste arrival order pays. The new rect goes straight into
       the entry: its old one stops meaning anything either way. */
    for (i = 0; i < before; i++) a->ents[i].resv = 0;   /* the untouched drop out here */
    for (hb = ORD_HBINS - 1; hb >= 1; hb--) {
        for (i = s_ordHead[hb]; i >= 0; i = s_ordNext[i]) {
            TAGPU_GAFENT* e = &a->ents[i];
            const int cw = cell_up(a, (int)e->w + 2 * p);
            if (cw > a->dim || hb > a->dim) continue;
            if (x + cw > a->dim) { y += sh; x = 0; sh = 0; }
            if (y + hb > a->dim) continue;      /* shorter bins may still fit */
            e->x = (unsigned short)(x + p);
            e->y = (unsigned short)(y + p);
            e->resv = 1;
            x += cw;
            if (hb > sh) sh = hb;
            kept++;
        }
    }

    /* An empty atlas that is also `full` could never take another frame, and
       the tallest-first layout has to be at least as good as the arrival
       order that got these entries in, so this cannot happen -- but the
       state it would leave is unrecoverable, so hand the caller back to the
       plain recycle rather than commit it. The rects written above belong to
       entries the recycle is about to make unreachable. */
    if (kept <= 0) return 0;

    /* Pass 2 -- compact the survivors down so `ents` stays dense (`n` is
       where atlas_insert puts the next one) and rebuild the hash over their
       new indices. `w <= i` throughout, so the copy never runs ahead of the
       read. */
    memset(a->hash, 0, sizeof a->hash);
    for (i = 0, w = 0; i < before; i++) {
        TAGPU_GAFENT* src = &a->ents[i];
        TAGPU_GAFENT* dst;
        int slot;
        if (!src->resv) continue;
        dst = &a->ents[w];
        if (dst != src) *dst = *src;
        dst->u0 = (float)dst->x / (float)a->dim;
        dst->v0 = (float)dst->y / (float)a->dim;
        dst->u1 = (float)(dst->x + dst->w) / (float)a->dim;
        dst->v1 = (float)(dst->y + dst->h) / (float)a->dim;
        dst->ok = 0;                    /* reserved until its next get paints it */
        dst->hit = 0;                   /* a fresh interval to prove it is still wanted */
        for (slot = (int)gaf_hash(dst->frame); a->hash[slot];
             slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) { }
        a->hash[slot] = ++w;
    }
    a->n = w;
    a->shelfX = x; a->shelfY = y; a->shelfH = sh;
    a->full = 0;
    a->gen++;                           /* every UV in the atlas has just moved */
    a->repacks++;
    /* the twin's rects moved with them: back to unpainted (job_clear clears
       level 0), and whatever was queued is dropped -- it re-queues as each
       reserved entry is painted. Unlike the recycle this happens once, which
       is what lets the twin converge at all while zoomed out. */
    if (a->job) { tagpu_rglsl_job_clear(a->job); twin_mips(a); }

    /* PART 3, the branch that says a second page is the only thing left.
       `wanted` is the set that was still being asked for; if the tallest-first
       layout could not hold all of it, no re-sort will, and repacking again
       would only re-decode the same frames to reach the same wall. Hold what
       we have rather than drop it for a rebuild that would place fewer.
       Note this compares against `wanted`, never against the previous
       repack's count: evicting the untouched legitimately lowers that count,
       and reading a drop as failure would latch the wall on a healthy atlas
       the first time a map went quiet. */
    if (kept < wanted) {
        a->repackWall = 1;
        a->full = 1;
        _snprintf(b, sizeof b,
                  "%s: atlas repack wall — %d of %d live frames fit one %d square tallest-first"
                  " (%d%% spanned); holding this layout, only a second page adds room",
                  a->tag ? a->tag : "gaf", kept, wanted, a->dim,
                  a->dim ? (y + sh) * 100 / a->dim : 0);
    } else {
        _snprintf(b, sizeof b,
                  "%s: atlas repacked — %d frames re-laid tallest-first (%d dropped unasked-for),"
                  " %d%% of the %d square, generation %u (repack %u)",
                  a->tag ? a->tag : "gaf", kept, wanted_skip,
                  a->dim ? (y + sh) * 100 / a->dim : 0, a->dim, a->gen, a->repacks);
    }
    glog(b);
    return 1;
}

/* Upload one entry's texels into the rect it has already been assigned, and
   finish it: the replicated border, the UVs, the tileability verdict and the
   restore queue. Shared by a first insertion and by the paint of an entry a
   repack reserved -- which is why it takes the rect from the entry rather
   than from the shelf cursor. */
static void atlas_paint(TAGPU_GAFATLAS* a, TAGPU_GAFENT* e, unsigned char ck,
                        const unsigned char* pixels)
{
    const int p = a->pad, w = e->w, h = e->h;
    const int cw = cell_up(a, w + 2 * p), ch = cell_up(a, h + 2 * p);
    const int x = e->x, y = e->y;
    int i;

    glBindTexture(GL_TEXTURE_2D, a->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Re-emit the frame with its outermost row and column repeated all
       round, `pad` deep. The border is what any sampler that reaches past
       the frame must land on: under GL_NEAREST that is the fragment whose
       centre falls exactly on the quad's far edge (its u interpolates to
       exactly u1, and floor(u1*dim) is one texel past the frame) — left
       unwritten that texel is whatever glTexImage2D(NULL) leaves, i.e.
       index 0, a real palette entry (black) rather than the frame's colour
       key, which is the black hairline down the right of every tree at
       zoom 0.25. Under a filtered sampler it is every edge fragment, which
       is why the border is on all four sides and not just the two the
       shelf packer used to leave spare; under a mipmapped one it is the
       whole 4-texel ring (tagpu_gaf.h `pad`). The cell's slack past the
       border, where the alignment rounds up (0..align-1 texels on the
       right and bottom), is filled with the same edge: at level 2 the
       far-edge sample of a frame whose width is 3 mod 4 takes a quarter
       of its weight from the level-2 texel that covers the slack, so
       unwritten slack would darken that column by a sixteenth. The whole
       cell is uploaded, and restore_enqueue has the OUT pass paint the
       twin's slack the same way. */
    {
        const int pw = cw, pr = cw - p - w, pb = ch - p - h;   /* right/bottom: p + slack */
        int k;
        for (i = 0; i < h; i++) {
            unsigned char* row = s_pad + (size_t)(i + p) * pw + p;
            memcpy(row, pixels + (size_t)i * w, (size_t)w);
            for (k = 1; k <= p; k++) row[-k] = row[0];
            for (k = 0; k < pr; k++) row[w + k] = row[w - 1];
        }
        for (k = 1; k <= p; k++)
            memcpy(s_pad + (size_t)(p - k) * pw, s_pad + (size_t)p * pw, (size_t)pw);
        for (k = 0; k < pb; k++)
            memcpy(s_pad + (size_t)(p + h + k) * pw, s_pad + (size_t)(p + h - 1) * pw, (size_t)pw);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x - p, y - p, cw, ch,
                        GL_RED, GL_UNSIGNED_BYTE, s_pad);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    e->u0 = (float)x / (float)a->dim;         e->v0 = (float)y / (float)a->dim;
    e->u1 = (float)(x + w) / (float)a->dim;   e->v1 = (float)(y + h) / (float)a->dim;
    e->ck = ck;
    /* decided here, while the pixels are still at hand: the tileability the
       restorer wrap-pads by (a key on an edge says no) */
    /* The tileability test asks about the ART, so it reads the engine's own
       table and not the palette the screen is shown with: a gamma-scaled
       palette stretches every colour distance by the same factor and moves
       tiles across the threshold (measured 2026-09-09 on the terrain's 5062:
       177 tiles wrap-padded at factor 1.5, 400 at 1.0). */
    {
        const unsigned char* art = tagpu_pal_engine();
        e->wrap = art ? (char)tagpu_rglsl_tileable(pixels, w, h, art, e->ck) : 0;
    }
    e->ok = 1; e->resv = 0;
    if (a->job) restore_enqueue(a, e);
}

/* the insertion shared by atlas_get (which decodes into s_dec first) and
   atlas_put (which is handed the bytes): `pixels` holds w*h indices */
static const TAGPU_GAFENT* atlas_insert(TAGPU_GAFATLAS* a, const void* g, const void* pix,
                                        int w, int h, unsigned char ck, const unsigned char* pixels)
{
    int slot;
    TAGPU_GAFENT* e;
    for (slot = (int)gaf_hash(g); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == g && c->pix == pix && c->w == w && c->h == h) {
            c->hit = 1;                 /* still wanted: survives the next repack */
            if (c->ok) return c;
            /* a repack reserved this rect and the caller is holding exactly
               the pixels it wants: paint it where it already sits */
            if (c->resv) { atlas_paint(a, c, ck, pixels); return c; }
            return NULL;
        }
    }
    /* every frame carries its OWN border of `pad` texels, so the shelf
       advances by the whole cell (w+2p / h+2p, rounded up to the alignment)
       rather than sharing one gutter between two neighbours */
    {
        const int p = a->pad, cw = cell_up(a, w + 2 * p), ch = cell_up(a, h + 2 * p);
        if (a->full || ch > a->dim) return NULL;
        if (a->n >= a->max) { a->full = 1; return NULL; }
        if (a->shelfX + cw > a->dim) { a->shelfY += a->shelfH; a->shelfX = 0; a->shelfH = 0; }
        if (a->shelfY + ch > a->dim) { a->full = 1; return NULL; }
        e = &a->ents[a->n];
        e->frame = g; e->pix = pix;
        e->w = (unsigned short)w; e->h = (unsigned short)h; e->ok = 0; e->resv = 0;
        e->hit = 1;                     /* asked for by definition: it is being inserted */
        e->x = (unsigned short)(a->shelfX + p);       /* inside the border */
        e->y = (unsigned short)(a->shelfY + p);
        a->hash[slot] = ++a->n;
        a->shelfX += cw;
        if (ch > a->shelfH) a->shelfH = ch;
    }
    atlas_paint(a, e, ck, pixels);
    return e;
}

const TAGPU_GAFENT* tagpu_gaf_atlas_get(TAGPU_GAFATLAS* a, const unsigned char* g)
{
    int w, h;
    const void* pix;
    const TAGPU_GAFENT* hit;
    if (!tagpu_gaf_atlas_create(a)) return NULL;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    /* tagpu_gaf_frame_sane already promises this of every caller's frame; the
       decode into s_dec and the guard-rail copy below both index off it */
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX) return NULL;
    pix = *(const void* const*)(g + TAGPU_GF_PIX);
    hit = tagpu_gaf_atlas_find(a, g, pix, w, h);
    /* the fast path is the one that runs hundreds of times a frame, so this is
       where an entry proves it is still part of the working set */
    if (hit) { ((TAGPU_GAFENT*)hit)->hit = 1; return hit; }
    /* a frame whose pixels are momentarily unreadable must stay retryable:
       claiming the slot here would cache the failure for the atlas's whole
       life, and the feature atlas is meant to live as long as the map */
    if (!tagpu_gaf_decode(g, w, h, s_dec)) return NULL;
    return atlas_insert(a, g, pix, w, h, g[TAGPU_GF_CK], s_dec);
}

const TAGPU_GAFENT* tagpu_gaf_atlas_put(TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                        int w, int h, unsigned char ck, const unsigned char* pixels)
{
    if (!tagpu_gaf_atlas_create(a)) return NULL;
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX || !pixels) return NULL;
    return atlas_insert(a, frame, pix, w, h, ck, pixels);
}

const TAGPU_GAFENT* tagpu_gaf_atlas_find(const TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                         int w, int h)
{
    int slot;
    if (!a->ents || !a->n) return NULL;
    for (slot = (int)gaf_hash(frame); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        const TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == frame && c->pix == pix && c->w == w && c->h == h)
            return c->ok ? c : NULL;
    }
    return NULL;
}
