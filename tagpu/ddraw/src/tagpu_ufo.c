/* tagpu_ufo.c — the HAPI archive writer. Rationale: tagpu_ufo.h.

   A port of `tools/hpipack.py`'s `build()` with method 1 and header key 0, so
   the two stay comparable: the layout is HPIPack's canonical one — the whole
   directory (records, entry arrays, names, file records) first, then the file
   payloads — which is what makes `directory size` in the header exactly the
   byte count a reader must load to walk the tree.

     header    "HAPI" | version 0x00010000 | dir size | header key 0 | root off
     dir rec   entry count | entries offset
     entry     name offset | data offset | flag        (9 bytes, 1 = subdir)
     file rec  payload offset | size | compression 1   (9 bytes)
     payload   u32 chunk sizes, then per chunk:
               "SQSH" v2 method1 enc1 csize dsize sum | body
     trailer   "Copyright 1997 Cavedog Entertainment", the last 36 bytes, raw

   Offsets stored in records are ABSOLUTE from the start of the file, which is
   why every one of them is a buffer offset plus HDR. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_log.h"

#include "tagpu_ufo.h"

#define HDR        20u          /* the header, before which nothing is stored */
#define CHUNK   65536u          /* the engine's decompressed chunk size       */

/* Read raw by 0x4BDD70 after seeking to the end, so it is appended AFTER the
   body and is not part of anything the header measures. */
static const char TRAILER[] = "Copyright 1997 Cavedog Entertainment";

static void ulog(const char* m)
{
    tagpu_log(m);
}

/* ---- a growable byte buffer -------------------------------------------- */
/* `bad` is sticky: one failed allocation poisons the buffer and every later
   call is a no-op, so the emit code below can run straight through and be
   checked once at the end rather than after every append. */
typedef struct { unsigned char* p; unsigned n, cap; int bad; } Buf;

static int bgrow(Buf* b, unsigned need)
{
    unsigned cap;
    unsigned char* q;

    if (b->bad) return 0;
    if (b->n + need <= b->cap) return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + need) cap *= 2;
    q = (unsigned char*)realloc(b->p, cap);
    if (!q) { b->bad = 1; return 0; }
    b->p = q;
    b->cap = cap;
    return 1;
}

/* Both return the BUFFER offset the bytes landed at (0 when the buffer is
   poisoned — harmless, since a poisoned buffer is never written out). */
static unsigned bput(Buf* b, const void* d, unsigned n)
{
    unsigned at = b->n;
    if (!bgrow(b, n)) return 0;
    memcpy(b->p + at, d, n);
    b->n += n;
    return at;
}

static unsigned bzero(Buf* b, unsigned n)
{
    unsigned at = b->n;
    if (!bgrow(b, n)) return 0;
    memset(b->p + at, 0, n);
    b->n += n;
    return at;
}

static void poke32(Buf* b, unsigned at, unsigned v)
{
    if (!b->bad && at + 4 <= b->n) memcpy(b->p + at, &v, 4);
}

/* ---- the payload -------------------------------------------------------- */

/* A method-1 (LZSS, 4 KiB window) stream that emits only literals: a tag byte
   whose bits are clear for a literal and set for a back-reference, eight
   literals per tag, then a set bit whose 2-byte back-reference has offset 0 —
   which is what ends the stream. Returns the byte count written to `out`,
   which the caller has sized at n + n/8 + 8. */
static unsigned lz77_store(const unsigned char* in, unsigned n, unsigned char* out)
{
    unsigned i = 0, k = 0, rest;

    while (n - i >= 8) {
        out[k++] = 0x00;
        memcpy(out + k, in + i, 8);
        k += 8;
        i += 8;
    }
    rest = n - i;
    out[k++] = rest ? (unsigned char)(0xFF << rest) : 1;  /* `rest` literals, then a set bit */
    memcpy(out + k, in + i, rest);
    k += rest;
    out[k++] = 0;                               /* the back-reference: offset 0, */
    out[k++] = 0;                               /* i.e. end of stream            */
    return k;
}

/* One file's payload: the u32 chunk-size table, then the SQSH chunks. The body
   of each chunk is encrypted as ((plain ^ i) + i) over the chunk's own index,
   and the checksum is the sum of the STORED (encrypted) bytes. */
static int pack_file(Buf* out, const unsigned char* data, unsigned size)
{
    unsigned nchunks = size ? (size + CHUNK - 1) / CHUNK : 1;
    unsigned table_at, c;
    unsigned char* comp;

    table_at = bzero(out, nchunks * 4);
    if (out->bad) return 0;

    comp = (unsigned char*)malloc(CHUNK + CHUNK / 8 + 8);
    if (!comp) { out->bad = 1; return 0; }

    for (c = 0; c < nchunks; c++) {
        unsigned off = c * CHUNK;
        unsigned len = size > off ? size - off : 0;
        unsigned cn, sum = 0, i;
        unsigned char head[19];

        if (len > CHUNK) len = CHUNK;
        cn = lz77_store(data + off, len, comp);

        for (i = 0; i < cn; i++) {
            comp[i] = (unsigned char)((comp[i] ^ (unsigned char)i) + (unsigned char)i);
            sum += comp[i];
        }

        memcpy(head, "SQSH", 4);
        head[4] = 2;                            /* version                     */
        head[5] = 1;                            /* method 1 = LZ77             */
        head[6] = 1;                            /* body is encrypted           */
        memcpy(head +  7, &cn,  4);
        memcpy(head + 11, &len, 4);
        memcpy(head + 15, &sum, 4);

        bput(out, head, sizeof head);
        bput(out, comp, cn);
        /* AFTER the appends: either may have realloc'd the buffer. */
        poke32(out, table_at + c * 4, 19 + cn);
    }

    free(comp);
    return !out->bad;
}

/* ---- the directory ------------------------------------------------------ */

/* The engine does not care about entry order; hpipack sorts case-insensitively so that the same inputs give the same bytes, and
   so does this. A separator sorts below every other byte: a name's folder ends before a longer name begins. */
static int foldc(int c)
{
    if (c == '/' || c == '\\') return 1;
    return c >= 'A' && c <= 'Z' ? c + 32 : (unsigned char)c;
}

typedef struct {
    const char* path;
    const void* data;
    unsigned    size;
    unsigned    frec;       /* the file record's offset in the directory */
} Ent;

static int entcmp(const void* pa, const void* pb)
{
    const char* a = ((const Ent*)pa)->path;
    const char* b = ((const Ent*)pb)->path;
    for (; *a && *b; a++, b++) {
        int ca = foldc(*a), cb = foldc(*b);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static unsigned component_len(const char* p)
{
    unsigned n = 0;
    while (p[n] && p[n] != '/' && p[n] != '\\') n++;
    return n;
}

/* whether two entries share the folder or file whose name starts at `off` in both */
static int same_component(const Ent* a, const Ent* b, unsigned off)
{
    unsigned n = component_len(a->path + off), i;
    if (n != component_len(b->path + off)) return 0;
    for (i = 0; i < n; i++)
        if (foldc(a->path[off + i]) != foldc(b->path[off + i])) return 0;
    return 1;
}

/* The directory holding entries [0, n), all alike up to `off`: its record, its list, then each child in turn, as
   hpipack's write_dir lays them out. The record's offset, or 0 when the paths are refused, the root being at HDR. */
static unsigned write_dir(Buf* dir, Ent* e, int n, unsigned off, int depth)
{
    unsigned rec, list, count = 0, at;
    int i, next;

    if (depth > TAGPU_UFO_DEPTH) return 0;
    for (i = 0; i < n; i = next) {
        for (next = i + 1; next < n && same_component(&e[i], &e[next], off); next++) {}
        count++;
    }
    rec = bzero(dir, 8);
    list = bzero(dir, 9u * count);
    poke32(dir, rec, count);
    poke32(dir, rec + 4, list + HDR);
    for (i = 0, at = 0; i < n; i = next, at++) {
        const char* name = e[i].path + off;
        unsigned len = component_len(name), ename = list + 9u * at, nameoff, data;
        static const char nul = 0;
        int is_dir = name[len] != 0;

        if (!len) return 0;
        for (next = i + 1; next < n && same_component(&e[i], &e[next], off); next++) {
            if (!is_dir) return 0;                      /* a file given twice, or also a folder */
            if (!e[next].path[off + len]) return 0;
        }
        nameoff = bput(dir, name, len);
        bput(dir, &nul, 1);
        if (is_dir) {
            data = write_dir(dir, e + i, next - i, off + len + 1, depth + 1);
            if (!data) return 0;
            data -= HDR;
        } else {
            e[i].frec = data = bzero(dir, 9);
            poke32(dir, data + 4, e[i].size);
            if (!dir->bad) dir->p[data + 8] = 1;        /* compression: method 1 */
        }
        poke32(dir, ename, nameoff + HDR);
        poke32(dir, ename + 4, data + HDR);
        if (!dir->bad) dir->p[ename + 8] = is_dir ? 1 : 0;
    }
    return rec + HDR;
}

int tagpu_ufo_write(const char* path, const TAGPU_UFO_FILE* files, int n)
{
    Ent*  ents;
    Buf   dir = { 0 }, body = { 0 };
    unsigned dir_size, total;
    unsigned char header[HDR];
    HANDLE h;
    DWORD  wrote = 0;
    int    ok, i;
    char   m[160];

    if (n < 0) return 0;
    ents = (Ent*)calloc(n ? (size_t)n : 1, sizeof(Ent));
    if (!ents) { ulog("ufo: out of memory"); return 0; }
    for (i = 0; i < n; i++) {
        ents[i].path = files[i].path;
        ents[i].data = files[i].data;
        ents[i].size = files[i].size;
    }
    qsort(ents, (size_t)n, sizeof(Ent), entcmp);

    if (!write_dir(&dir, ents, n, 0, 0)) {
        _snprintf(m, sizeof m, "ufo: refusing the file list of %s: a path is empty, given twice, both a file and a folder, or too deep", path);
        m[sizeof m - 1] = 0;
        ulog(m);
        free(dir.p);
        free(ents);
        return 0;
    }

    /* the payloads, and the offset each file record could not know yet */
    dir_size = HDR + dir.n;
    for (i = 0; i < n && !dir.bad && !body.bad; i++) {
        poke32(&dir, ents[i].frec, dir_size + body.n);
        pack_file(&body, (const unsigned char*)ents[i].data, ents[i].size);
    }
    free(ents);

    if (dir.bad || body.bad) { free(dir.p); free(body.p); ulog("ufo: out of memory"); return 0; }

    memcpy(header, "HAPI", 4);
    { unsigned v = 0x00010000u; memcpy(header + 4, &v, 4); }
    memcpy(header + 8, &dir_size, 4);
    { unsigned z = 0; memcpy(header + 12, &z, 4); }      /* header key 0: plain  */
    { unsigned r = HDR;  memcpy(header + 16, &r, 4); }   /* the root record      */

    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf(m, sizeof m, "ufo: cannot create %s (err %lu)", path, GetLastError());
        m[sizeof m - 1] = 0;
        ulog(m);
        free(dir.p); free(body.p);
        return 0;
    }
    ok = WriteFile(h, header, HDR, &wrote, NULL) && wrote == HDR;
    ok = ok && WriteFile(h, dir.p, dir.n, &wrote, NULL) && wrote == dir.n;
    ok = ok && WriteFile(h, body.p, body.n, &wrote, NULL) && wrote == body.n;
    ok = ok && WriteFile(h, TRAILER, sizeof TRAILER - 1, &wrote, NULL)
            && wrote == sizeof TRAILER - 1;
    CloseHandle(h);

    total = HDR + dir.n + body.n + (unsigned)(sizeof TRAILER - 1);
    free(dir.p);
    free(body.p);

    if (!ok) { ulog("ufo: write failed"); return 0; }
    _snprintf(m, sizeof m, "ufo: wrote %s - %d file%s, %u bytes", path, n, n == 1 ? "" : "s", total);
    m[sizeof m - 1] = 0;
    ulog(m);
    return 1;
}
