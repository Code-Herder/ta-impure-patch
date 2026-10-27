/* tagpu_hpi.c -- TA's HAPI archives, read-only. Contract and format: tagpu_hpi.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lodepng.h"
#include "tagpu_hpi.h"

#define HDR_SIZE    20u
#define MARKER_HAPI 0x49504148u         /* "HAPI" */
#define MARKER_BANK 0x4B4E4142u         /* "BANK": a saved game's archive */
#define MARKER_SQSH 0x48535153u         /* "SQSH" */
#define SQSH_HDR    19u
#define ENTRY_SIZE  9u
#define DIRMAX      (16u << 20)         /* a directory block this reader takes: the stock archives'
                                           largest is under 1 MB */
#define EF_DIR      0x01u
#define EF_SHADOW   0x02u

typedef struct {
    uint32_t name;          /* the name's offset in the decrypted directory copy                 */
    uint32_t data;          /* a directory: its first child's index; a file: its data's offset   */
    uint32_t size;          /* a directory: its child count; a file: its size, decompressed      */
    uint8_t  flag;          /* EF_*                                                               */
    uint8_t  comp;          /* a file: 0 stored, otherwise chunked                                */
    uint8_t  depth;
} ENT;

struct TAGPU_HPI {
    TAGPU_HPISRC   src;
    uint8_t        key;
    uint32_t       dstart, dend;
    unsigned char* dir;     /* [dstart, dend), decrypted                                          */
    ENT*           ent;
    int            nent;
};

static void say(char* why, size_t nwhy, const char* fmt, const char* a, unsigned long b)
{
    if (why && nwhy) snprintf(why, nwhy, fmt, a, b);
}

static uint32_t rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The obfuscation is by FILE position, so a range is decrypted knowing where it was read from. */
static void unkey(uint8_t key, uint32_t pos, unsigned char* b, uint32_t n)
{
    uint32_t i;
    if (!key) return;
    for (i = 0; i < n; i++) b[i] = (unsigned char)(((pos + i) ^ key ^ ~b[i]) & 0xFF);
}

static int src_read(const TAGPU_HPI* a, uint32_t off, void* dst, uint32_t n)
{
    if ((uint64_t)off + n > a->src.size) return 0;
    if (!n) return 1;
    if (!a->src.read(a->src.ctx, off, dst, n)) return 0;
    unkey(a->key, off, (unsigned char*)dst, n);
    return 1;
}

int tagpu_hpi_stricmp(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0;; i++) {
        unsigned ca = (unsigned char)a[i], cb = i < n ? (unsigned char)b[i] : 0u;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

/* the bytes [off, off + n) of the directory block, when they lie inside it */
static const unsigned char* dir_at(const TAGPU_HPI* a, uint32_t off, uint32_t n)
{
    if (off < a->dstart || (uint64_t)off + n > a->dend) return NULL;
    return a->dir + (off - a->dstart);
}

/* THE WALK, breadth first, so a directory's children take consecutive indices. Every list, name and
   file record is bounded against the directory block; `seen` marks each entry's first byte, so an
   entry two lists share, or a list that leads back to an ancestor, is refused rather than walked
   again; the entry array is sized for the most entries the block can hold, so it cannot fill. */
static int walk(TAGPU_HPI* a, char* why, size_t nwhy)
{
    const uint32_t span = a->dend - a->dstart;
    const int cap = (int)(span / ENTRY_SIZE) + 1;
    unsigned char* seen = (unsigned char*)calloc(span / 8u + 1u, 1);
    int i;
    if (!seen || !(a->ent = (ENT*)calloc((size_t)cap, sizeof *a->ent))) {
        free(seen);
        say(why, nwhy, "%sout of memory for a directory of %lu bytes", "", (unsigned long)span);
        return 0;
    }
    a->ent[0].flag = EF_DIR;
    a->ent[0].data = a->dstart;         /* the root's {count, list}: replaced below */
    a->nent = 1;
    for (i = 0; i < a->nent; i++) {
        ENT* d = &a->ent[i];
        const unsigned char* h;
        uint32_t count, list, k;
        if (!(d->flag & EF_DIR)) continue;
        if (!(h = dir_at(a, d->data, 8))) {
            say(why, nwhy, "%sa directory's header at 0x%lx lies outside the directory block", "", (unsigned long)d->data);
            goto fail;
        }
        count = rd32(h);
        list = rd32(h + 4);
        if (count > (uint32_t)(cap - a->nent) || !dir_at(a, list, count * ENTRY_SIZE)) {
            say(why, nwhy, "%sa directory's list of %lu entries lies outside the directory block", "", (unsigned long)count);
            goto fail;
        }
        if (count && d->depth >= TAGPU_HPI_DEPTH) {
            say(why, nwhy, "%sthe directory is deeper than %lu levels", "", (unsigned long)TAGPU_HPI_DEPTH);
            goto fail;
        }
        d->data = (uint32_t)a->nent;
        d->size = count;
        for (k = 0; k < count; k++) {
            const uint32_t at = list + k * ENTRY_SIZE, rel = at - a->dstart;
            const unsigned char* e = dir_at(a, at, ENTRY_SIZE);
            const unsigned char* nm;
            ENT* c = &a->ent[a->nent];
            uint32_t name = rd32(e), data = rd32(e + 4), len;
            if (seen[rel >> 3] & (1u << (rel & 7))) {
                say(why, nwhy, "%sthe entry at 0x%lx is reached twice (a cycle or a shared list)", "", (unsigned long)at);
                goto fail;
            }
            seen[rel >> 3] |= (unsigned char)(1u << (rel & 7));
            if (!(nm = dir_at(a, name, 1))) {
                say(why, nwhy, "%sa name at 0x%lx lies outside the directory block", "", (unsigned long)name);
                goto fail;
            }
            for (len = 0; len < TAGPU_HPI_NAMEMAX && name + len < a->dend && nm[len]; len++) {}
            if (len >= TAGPU_HPI_NAMEMAX || name + len >= a->dend) {
                say(why, nwhy, "%sthe name at 0x%lx is not terminated within its bound or the block", "", (unsigned long)name);
                goto fail;
            }
            c->name = name - a->dstart;
            c->flag = (uint8_t)(e[8] & EF_DIR);      /* bit 1 is the mount's, cleared as 0x4BE320 clears it */
            c->depth = (uint8_t)(d->depth + 1);
            if (c->flag & EF_DIR) {
                c->data = data;                      /* its {count, list}, walked in its turn */
            } else {
                const unsigned char* r = dir_at(a, data, 9);
                if (!r) {
                    say(why, nwhy, "%sa file record at 0x%lx lies outside the directory block", "", (unsigned long)data);
                    goto fail;
                }
                c->data = rd32(r);
                c->size = rd32(r + 4);
                c->comp = r[8];
            }
            a->nent++;
        }
    }
    free(seen);
    return 1;
fail:
    free(seen);
    return 0;
}

TAGPU_HPI* tagpu_hpi_open(const TAGPU_HPISRC* src, char* why, size_t nwhy)
{
    TAGPU_HPI* a = (TAGPU_HPI*)calloc(1, sizeof *a);
    unsigned char h[HDR_SIZE];
    uint32_t version, hkey;
    if (why && nwhy) why[0] = 0;
    if (!a) { say(why, nwhy, "%sout of memory", "", 0); return NULL; }
    a->src = *src;
    if (a->src.size < HDR_SIZE || !a->src.read(a->src.ctx, 0, h, HDR_SIZE)) {
        say(why, nwhy, "%sshorter than the %lu-byte header", "", (unsigned long)HDR_SIZE);
        goto fail;
    }
    if (rd32(h) != MARKER_HAPI) { say(why, nwhy, "%snot a HAPI archive", "", 0); goto fail; }
    version = rd32(h + 4);
    if (version == MARKER_BANK) { say(why, nwhy, "%sa saved game's bank, not assets", "", 0); goto fail; }
    a->dend = rd32(h + 8);
    hkey = rd32(h + 12);
    a->dstart = rd32(h + 16);
    a->key = hkey ? (uint8_t)(~((hkey * 4u) | (hkey >> 6)) & 0xFFu) : 0;
    if (a->dstart < HDR_SIZE || a->dend > a->src.size || a->dstart > a->dend || a->dend - a->dstart < 8) {
        say(why, nwhy, "%sthe directory [start, end) is not inside the file's %lu bytes", "", (unsigned long)a->src.size);
        goto fail;
    }
    if (a->dend - a->dstart > DIRMAX) {
        say(why, nwhy, "%sa directory of %lu bytes, past the reader's bound", "", (unsigned long)(a->dend - a->dstart));
        goto fail;
    }
    if (!(a->dir = (unsigned char*)malloc(a->dend - a->dstart)) || !src_read(a, a->dstart, a->dir, a->dend - a->dstart)) {
        say(why, nwhy, "%sthe directory could not be read", "", 0);
        goto fail;
    }
    if (!walk(a, why, nwhy)) goto fail;
    return a;
fail:
    tagpu_hpi_close(a);
    return NULL;
}

void tagpu_hpi_close(TAGPU_HPI* a)
{
    if (!a) return;
    free(a->dir);
    free(a->ent);
    free(a);
}

static const ENT* ent(const TAGPU_HPI* a, int e)
{
    return a && e >= 0 && e < a->nent ? &a->ent[e] : NULL;
}

int tagpu_hpi_count(const TAGPU_HPI* a)
{
    return a ? a->nent : 0;
}

int tagpu_hpi_children(const TAGPU_HPI* a, int dir)
{
    const ENT* d = ent(a, dir);
    return d && (d->flag & EF_DIR) ? (int)d->size : 0;
}

int tagpu_hpi_child(const TAGPU_HPI* a, int dir, int i)
{
    const ENT* d = ent(a, dir);
    if (!d || !(d->flag & EF_DIR) || i < 0 || (uint32_t)i >= d->size) return -1;
    return (int)d->data + i;
}

const char* tagpu_hpi_name(const TAGPU_HPI* a, int e)
{
    const ENT* x = ent(a, e);
    return x && e > 0 ? (const char*)a->dir + x->name : "";
}

int tagpu_hpi_is_dir(const TAGPU_HPI* a, int e)
{
    const ENT* x = ent(a, e);
    return x && (x->flag & EF_DIR);
}

uint32_t tagpu_hpi_size(const TAGPU_HPI* a, int e)
{
    const ENT* x = ent(a, e);
    return x && !(x->flag & EF_DIR) ? x->size : 0;
}

int tagpu_hpi_shadowed(const TAGPU_HPI* a, int e)
{
    const ENT* x = ent(a, e);
    return x && (x->flag & EF_SHADOW);
}

void tagpu_hpi_shadow(TAGPU_HPI* a, int e)
{
    if (ent(a, e)) a->ent[e].flag |= EF_SHADOW;
}

int tagpu_hpi_lookup(const TAGPU_HPI* a, int dir, const char* name, size_t len)
{
    int i, n = tagpu_hpi_children(a, dir);
    char key[TAGPU_HPI_NAMEMAX];
    if (len >= sizeof key) return -1;
    memcpy(key, name, len);
    key[len] = 0;
    for (i = n - 1; i >= 0; i--) {
        const int c = tagpu_hpi_child(a, dir, i);
        if (!tagpu_hpi_stricmp(key, tagpu_hpi_name(a, c), TAGPU_HPI_NAMEMAX)) return c;
    }
    return -1;
}

int tagpu_hpi_find(const TAGPU_HPI* a, const char* path)
{
    int at = 0;
    const char* p = path;
    if (!a) return -1;
    for (;;) {
        const char* end = strchr(p, '\\');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if ((at = tagpu_hpi_lookup(a, at, p, len)) < 0) return -1;
        if (!end) return at;
        if (!tagpu_hpi_is_dir(a, at)) return -1;
        p = end + 1;
    }
}

/* ---- the data ----------------------------------------------------------------------------- */

/* tools/ta3do's lz77_decompress: LZSS over a 4 KiB window starting at 1, a tag byte read least
   significant bit first, a CLEAR bit a literal, a SET bit a two-byte reference (offset = v >> 4,
   count = (v & 15) + 2), offset 0 the end. It stops at `want` bytes; a reference that would write
   past `want` is refused (-1), where the Python returns the overrun and its caller fails on the
   total. Returns the bytes written. */
static long lz77(const unsigned char* src, uint32_t n, unsigned char* out, uint32_t want)
{
    unsigned char win[4096];
    uint32_t wpos = 1, i = 0, o = 0;
    memset(win, 0, sizeof win);
    while (i < n && o < want) {
        const unsigned tag = src[i++];
        unsigned bit;
        for (bit = 0; bit < 8; bit++) {
            if (!(tag & (1u << bit))) {
                if (i >= n) return (long)o;
                out[o] = win[wpos] = src[i++];
                o++;
                wpos = (wpos + 1) & 0xFFF;
            } else {
                uint32_t v, from, count, k;
                if (i + 1 >= n) return (long)o;
                v = (uint32_t)src[i] | (uint32_t)src[i + 1] << 8;
                i += 2;
                if (!(from = v >> 4)) return (long)o;
                count = (v & 0x0F) + 2;
                if (o + count > want) return -1;
                for (k = 0; k < count; k++) {
                    out[o++] = win[wpos] = win[from];
                    from = (from + 1) & 0xFFF;
                    wpos = (wpos + 1) & 0xFFF;
                }
            }
            if (o >= want) return (long)o;
        }
    }
    return (long)o;
}

/* chunk `k` of a file whose chunks start at `pos`, `stored` bytes long, decompressed into `out`,
   which must come to exactly `want` bytes */
static int chunk(const TAGPU_HPI* a, uint32_t pos, uint32_t stored, unsigned char* out, uint32_t want,
                 char* why, size_t nwhy)
{
    unsigned char* b;
    uint32_t comp, dec, i;
    int ok = 0;
    if (stored < SQSH_HDR || stored > TAGPU_HPI_CHUNKMAX) {
        say(why, nwhy, "%sa chunk of %lu stored bytes, outside the reader's bounds", "", (unsigned long)stored);
        return 0;
    }
    if (!(b = (unsigned char*)malloc(stored))) { say(why, nwhy, "%sout of memory", "", 0); return 0; }
    if (!src_read(a, pos, b, stored)) {
        say(why, nwhy, "%sa chunk at 0x%lx lies past the archive", "", (unsigned long)pos);
        goto done;
    }
    comp = rd32(b + 7);
    dec = rd32(b + 11);
    if (rd32(b) != MARKER_SQSH) { say(why, nwhy, "%sa chunk without its SQSH marker", "", 0); goto done; }
    if (comp > stored - SQSH_HDR) {
        say(why, nwhy, "%sa chunk's body of %lu bytes runs past its stored length", "", (unsigned long)comp);
        goto done;
    }
    if (dec != want) {
        say(why, nwhy, "%sa chunk says %lu bytes where the file's size gives it another count", "", (unsigned long)dec);
        goto done;
    }
    if (b[6])
        for (i = 0; i < comp; i++)
            b[SQSH_HDR + i] = (unsigned char)((unsigned char)(b[SQSH_HDR + i] - (unsigned char)i) ^ (unsigned char)i);
    if (b[5] == 1) {
        const long got = lz77(b + SQSH_HDR, comp, out, want);
        if (got != (long)want) {
            say(why, nwhy, got < 0 ? "%sLZ77 ran past the chunk's size" : "%sLZ77 came to %lu bytes short of the chunk's size",
                "", (unsigned long)(got < 0 ? 0 : want - (uint32_t)got));
            goto done;
        }
    } else if (b[5] == 2) {
        unsigned char* z = NULL;
        size_t zn = 0;
        LodePNGDecompressSettings s;
        unsigned err;
        lodepng_decompress_settings_init(&s);
        err = lodepng_zlib_decompress(&z, &zn, b + SQSH_HDR, comp, &s);
        if (err || zn != want) {
            free(z);
            say(why, nwhy, err ? "%szlib refused a chunk (lodepng error %lu)" : "%szlib came to another size than the chunk's (%lu bytes)",
                "", err ? (unsigned long)err : (unsigned long)zn);
            goto done;
        }
        memcpy(out, z, want);
        free(z);
    } else {
        say(why, nwhy, "%sa chunk of compression method %lu", "", (unsigned long)b[5]);
        goto done;
    }
    ok = 1;
done:
    free(b);
    return ok;
}

int tagpu_hpi_read(const TAGPU_HPI* a, int e, uint32_t off, uint32_t n, void* dst, char* why, size_t nwhy)
{
    const ENT* f = ent(a, e);
    unsigned char* tab = NULL;
    unsigned char* buf = NULL;
    uint32_t nch, first, last, k, pos;
    int ok = 0;
    if (why && nwhy) why[0] = 0;
    if (!f || (f->flag & EF_DIR)) { say(why, nwhy, "%snot a file", "", 0); return 0; }
    if (f->size > TAGPU_HPI_FILEMAX) {
        say(why, nwhy, "%sa file of %lu bytes, past the reader's bound", "", (unsigned long)f->size);
        return 0;
    }
    if ((uint64_t)off + n > f->size) {
        say(why, nwhy, "%sa read past the file's %lu bytes", "", (unsigned long)f->size);
        return 0;
    }
    if (!n) return 1;
    if (!f->comp) {
        if ((uint64_t)f->data + f->size > a->src.size || !src_read(a, f->data + off, dst, n)) {
            say(why, nwhy, "%sthe file's data at 0x%lx lies past the archive", "", (unsigned long)f->data);
            return 0;
        }
        return 1;
    }
    nch = (f->size + TAGPU_HPI_CHUNK - 1) / TAGPU_HPI_CHUNK;
    if (!(tab = (unsigned char*)malloc((size_t)nch * 4u)) || !(buf = (unsigned char*)malloc(TAGPU_HPI_CHUNK))) {
        say(why, nwhy, "%sout of memory", "", 0);
        goto done;
    }
    if (!src_read(a, f->data, tab, nch * 4u)) {
        say(why, nwhy, "%sthe file's chunk table at 0x%lx lies past the archive", "", (unsigned long)f->data);
        goto done;
    }
    first = off / TAGPU_HPI_CHUNK;
    last = (off + n - 1) / TAGPU_HPI_CHUNK;
    pos = f->data + nch * 4u;
    for (k = 0; k <= last; k++) {
        const uint32_t stored = rd32(tab + 4u * k);
        if ((uint64_t)pos + stored > a->src.size) {
            say(why, nwhy, "%sa chunk at 0x%lx runs past the archive", "", (unsigned long)pos);
            goto done;
        }
        if (k >= first) {
            const uint32_t c0 = k * TAGPU_HPI_CHUNK;
            const uint32_t want = f->size - c0 < TAGPU_HPI_CHUNK ? f->size - c0 : TAGPU_HPI_CHUNK;
            const uint32_t lo = off > c0 ? off - c0 : 0;
            const uint32_t hi = off + n - c0 < want ? off + n - c0 : want;
            if (!chunk(a, pos, stored, buf, want, why, nwhy)) goto done;
            memcpy((unsigned char*)dst + (c0 + lo - off), buf + lo, hi - lo);
        }
        pos += stored;
    }
    ok = 1;
done:
    free(tab);
    free(buf);
    return ok;
}
