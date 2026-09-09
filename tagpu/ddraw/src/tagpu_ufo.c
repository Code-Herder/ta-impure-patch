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

#include "tagpu_ufo.h"

#define HDR        20u          /* the header, before which nothing is stored */
#define CHUNK   65536u          /* the engine's decompressed chunk size       */
#define MAXFILES   16
#define MAXDIRS     8

/* Read raw by 0x4BDD70 after seeking to the end, so it is appended AFTER the
   body and is not part of anything the header measures. */
static const char TRAILER[] = "Copyright 1997 Cavedog Entertainment";

static void ulog(const char* m)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
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
    out[k++] = (unsigned char)(0xFF << rest);   /* `rest` literals, then a set bit */
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

/* The engine does not care about entry order; hpipack sorts case-insensitively
   so that the same inputs give the same bytes, and so does this. */
static int namecmp(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        int cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Split "guis/render.gui" into "guis" and "render.gui". 0 unless there is
   exactly one separator with something on both sides. */
static int split(const char* path, char* dir, unsigned dcap, const char** base)
{
    const char* slash = 0;
    const char* p;
    unsigned n;

    if (!path || !*path) return 0;
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            if (slash) return 0;                /* two levels: refused, not guessed */
            slash = p;
        }
    }
    if (!slash || slash == path || !slash[1]) return 0;
    n = (unsigned)(slash - path);
    if (n + 1 > dcap) return 0;
    memcpy(dir, path, n);
    dir[n] = 0;
    *base = slash + 1;
    return 1;
}

int tagpu_ufo_write(const char* path, const TAGPU_UFO_FILE* files, int n)
{
    char  dirs[MAXDIRS][32];
    int   order[MAXFILES];                      /* file indices, grouped by dir */
    char  fdir[MAXFILES][32];
    const char* fbase[MAXFILES];
    unsigned frec[MAXFILES];                    /* each file record's offset    */
    int   ndirs = 0, i, j, k;
    Buf   dir = { 0 }, body = { 0 };
    unsigned root_ent, dir_size, total;
    unsigned char header[HDR];
    HANDLE h;
    DWORD  wrote = 0;
    int    ok;
    char   m[160];

    if (n <= 0 || n > MAXFILES) return 0;

    /* 1. split every path, and collect the distinct directories */
    for (i = 0; i < n; i++) {
        if (!split(files[i].path, fdir[i], sizeof fdir[i], &fbase[i])) {
            _snprintf(m, sizeof m, "ufo: refusing %s - paths must be dir/file",
                      files[i].path ? files[i].path : "(null)");
            m[sizeof m - 1] = 0;
            ulog(m);
            return 0;
        }
        for (j = 0; j < ndirs; j++) if (!namecmp(dirs[j], fdir[i])) break;
        if (j == ndirs) {
            if (ndirs == MAXDIRS) return 0;
            lstrcpynA(dirs[ndirs++], fdir[i], sizeof dirs[0]);
        }
    }

    /* 2. sort the directories, and the files inside each */
    for (i = 1; i < ndirs; i++) {
        char t[32];
        lstrcpynA(t, dirs[i], sizeof t);
        for (j = i; j > 0 && namecmp(dirs[j - 1], t) > 0; j--)
            lstrcpynA(dirs[j], dirs[j - 1], sizeof dirs[0]);
        lstrcpynA(dirs[j], t, sizeof dirs[0]);
    }

    /* 3. the root record, then one subdirectory per `dirs` entry */
    bzero(&dir, 8);                             /* the root record, at offset 0 */
    root_ent = bzero(&dir, 9u * (unsigned)ndirs);

    for (k = 0; k < ndirs; k++) {
        unsigned dname, sub_rec, sub_ent, e;
        int count = 0;

        for (i = 0; i < n; i++)
            if (!namecmp(fdir[i], dirs[k])) order[count++] = i;
        for (i = 1; i < count; i++) {           /* insertion sort by base name  */
            int t = order[i];
            for (j = i; j > 0 && namecmp(fbase[order[j - 1]], fbase[t]) > 0; j--)
                order[j] = order[j - 1];
            order[j] = t;
        }

        dname   = bput(&dir, dirs[k], (unsigned)lstrlenA(dirs[k]) + 1);
        sub_rec = bzero(&dir, 8);
        sub_ent = bzero(&dir, 9u * (unsigned)count);

        for (i = 0; i < count; i++) {
            int f = order[i];
            unsigned fname = bput(&dir, fbase[f], (unsigned)lstrlenA(fbase[f]) + 1);
            frec[f] = bzero(&dir, 9);
            poke32(&dir, frec[f] + 4, files[f].size);
            if (!dir.bad) dir.p[frec[f] + 8] = 1;        /* compression: method 1 */
            e = sub_ent + 9u * (unsigned)i;
            poke32(&dir, e,     fname + HDR);
            poke32(&dir, e + 4, frec[f] + HDR);
            if (!dir.bad) dir.p[e + 8] = 0;              /* a file, not a subdir  */
        }

        poke32(&dir, sub_rec,     (unsigned)count);
        poke32(&dir, sub_rec + 4, sub_ent + HDR);

        e = root_ent + 9u * (unsigned)k;
        poke32(&dir, e,     dname + HDR);
        poke32(&dir, e + 4, sub_rec + HDR);
        if (!dir.bad) dir.p[e + 8] = 1;                  /* a subdirectory        */
    }
    poke32(&dir, 0, (unsigned)ndirs);
    poke32(&dir, 4, root_ent + HDR);

    /* 4. the payloads, and the offset each file record could not know yet */
    dir_size = HDR + dir.n;
    for (i = 0; i < n && !dir.bad && !body.bad; i++) {
        poke32(&dir, frec[i], dir_size + body.n);
        pack_file(&body, (const unsigned char*)files[i].data, files[i].size);
    }

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
    _snprintf(m, sizeof m, "ufo: wrote %s - %d file%s in %d dir%s, %u bytes",
              path, n, n == 1 ? "" : "s", ndirs, ndirs == 1 ? "" : "s", total);
    m[sizeof m - 1] = 0;
    ulog(m);
    return 1;
}
