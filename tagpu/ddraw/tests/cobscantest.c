/* cobscantest.c -- the C half of tests/cobscantest.py: the real tagpu_cobscan.c, compiled
   natively with the sanitizers, run over every stock unit's script and model.

   Input on stdin, one record a unit, every integer a little-endian u32:
     name length, name; script length, script (the file's bytes); node count, then each node's
     name length and name, in the engine's piece order for a new unit (0x45AEC0).
   Nothing is read from the game's files here: cobscantest.py reads them from the install and
   pipes them in, and nothing is written to disk.

   One line out a record, tab-separated:
     unit, hides (tagpu_cobscan_hides' return), the hidden COB pieces' names (comma-separated,
     lower case, in piece order), mask (tagpu_cobscan_mask's return), the hidden nodes' names
     (the same form, in walk order), and `same` or `DIFF:<what>`: whether the script relocated
     as 0x4B2450 leaves it, at two bases (one that wraps), gives the same hides, pieces and mask
     as the file's bytes at base 0.
   The mask runs over a walk that is the engine order reversed, so a bit lands by node address
   and not by position.

   Then every script again, truncated to every length up to 160 bytes and every 13th after,
   and with 48 seeded dwords overwritten, each copy in an allocation of exactly its length: no
   answer is checked, the sanitizers are -- every read is bounded by the length given.

   Exit 0 when everything ran; the comparison with tools/ta3do's hidden_at_create is
   cobscantest.py's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_cobscan.h"

#define MAXNODES 256

static int rd_u32(uint32_t* v)
{
    unsigned char b[4];
    if (fread(b, 1, 4, stdin) != 4) return 0;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

static char* rd_blob(uint32_t* n)
{
    char* p;
    if (!rd_u32(n) || *n > (64u << 20)) return NULL;
    p = (char*)malloc(*n ? *n : 1);
    if (!p || fread(p, 1, *n, stdin) != *n) { free(p); return NULL; }
    return p;
}

static void put_le(char* p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get_le(const char* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* the script as 0x4B2450 leaves it at address `base`: the four header offsets and every entry
   of the two name tables moved by `base` (0x4B24A7..0x4B2527); 0 when a table lies outside the
   script, which then has no relocated twin to compare */
static int relocate(char* c, uint32_t n, uint32_t base)
{
    uint32_t ns, np, sn, pn, i;
    if (n < 0x2C) return 0;
    ns = get_le(c + 0x04); np = get_le(c + 0x08);
    sn = get_le(c + 0x1C); pn = get_le(c + 0x20);
    if (ns > n / 4 || np > n / 4 || sn > n || pn > n || (n - sn) / 4 < ns || (n - pn) / 4 < np)
        return 0;
    for (i = 0; i < ns; i++) put_le(c + sn + 4 * i, get_le(c + sn + 4 * i) + base);
    for (i = 0; i < np; i++) put_le(c + pn + 4 * i, get_le(c + pn + 4 * i) + base);
    for (i = 0x18; i <= 0x24; i += 4) put_le(c + i, get_le(c + i) + base);
    return 1;
}

typedef struct {
    int hides, mask;
    uint32_t hid[TAGPU_COB_HIDWORDS];
    uint32_t bits[MAXNODES / 32];
} Answer;

static char s_marker[MAXNODES];   /* the nodes' addresses: one byte each, never read */

static void answer(const char* cob, uint32_t n, uint32_t base, char* const* names, int nn,
                   Answer* a)
{
    const char* prim[MAXNODES];
    const char* pname[MAXNODES];
    const char* walk[MAXNODES];
    int k;
    memset(a, 0, sizeof *a);
    a->hides = tagpu_cobscan_hides(cob, n, base, a->hid);
    for (k = 0; k < nn; k++) {
        prim[k] = &s_marker[k];
        pname[k] = names[k];
        walk[nn - 1 - k] = &s_marker[k];
    }
    a->mask = tagpu_cobscan_mask(cob, n, base, prim, pname, nn > 0 ? nn : -1, walk, nn, a->bits);
}

static void print_list(const char* cob, uint32_t n, const Answer* a, char* const* names, int nn)
{
    int k, first = 1;
    for (k = 0; a->hides > 0 && k < a->hides; k++) {
        unsigned room;
        const char* nm;
        if (!(a->hid[k >> 5] >> (k & 31) & 1)) continue;
        nm = tagpu_cobscan_piece(cob, n, 0, k, &room);
        printf("%s", first ? "" : ",");
        first = 0;
        if (!nm) { printf("?%d", k); continue; }
        for (; room && *nm; nm++, room--) putchar(*nm >= 'A' && *nm <= 'Z' ? *nm + 32 : *nm);
    }
    printf("\t%d\t", a->mask);
    first = 1;
    for (k = 0; a->mask > 0 && k < nn; k++) {
        const char* nm;
        if (!(a->bits[k >> 5] >> (k & 31) & 1)) continue;
        printf("%s", first ? "" : ",");
        first = 0;
        for (nm = names[nn - 1 - k]; *nm; nm++) putchar(*nm >= 'A' && *nm <= 'Z' ? *nm + 32 : *nm);
    }
}

static int same(const Answer* a, const Answer* b)
{
    return a->hides == b->hides && a->mask == b->mask &&
           !memcmp(a->hid, b->hid, sizeof a->hid) && !memcmp(a->bits, b->bits, sizeof a->bits);
}

/* the script at exactly `len` bytes, its answers taken and thrown away */
static void probe(const char* src, uint32_t len, char* const* names, int nn)
{
    char* c = (char*)malloc(len ? len : 1);
    Answer a;
    if (!c) exit(2);
    memcpy(c, src, len);
    answer(len ? c : NULL, len, 0, names, nn, &a);
    free(c);
}

int main(void)
{
    char** scripts = NULL;
    uint32_t* sizes = NULL;
    char*** nodes = NULL;
    int* counts = NULL;
    int units = 0, cap = 0, u, diffs = 0;
    uint32_t seed = 0x2545F491u;
    for (;;) {
        uint32_t ln, cn, nn, k;
        char* name = rd_blob(&ln);
        char* cob;
        char** nm;
        Answer file, moved;
        const char* why = NULL;
        static const uint32_t BASES[2] = { 0x00A00000u, 0xFFFFFF00u };
        int b;
        if (!name) break;
        if (!(cob = rd_blob(&cn)) || !rd_u32(&nn) || nn > MAXNODES) return 2;
        nm = (char**)calloc(nn ? nn : 1, sizeof *nm);
        for (k = 0; k < nn; k++) {
            uint32_t l;
            char* s = rd_blob(&l);
            if (!s || !(nm[k] = (char*)malloc(l + 1))) return 2;
            memcpy(nm[k], s, l);
            nm[k][l] = 0;
            free(s);
        }
        answer(cob, cn, 0, nm, (int)nn, &file);
        for (b = 0; b < 2 && !why; b++) {
            char* c = (char*)malloc(cn ? cn : 1);
            memcpy(c, cob, cn);
            if (relocate(c, cn, BASES[b])) {
                answer(c, cn, BASES[b], nm, (int)nn, &moved);
                if (!same(&file, &moved)) why = b ? "wrapped base" : "base";
            }
            free(c);
        }
        printf("%.*s\t%d\t", (int)ln, name, file.hides);
        print_list(cob, cn, &file, nm, (int)nn);
        printf("\t%s%s\n", why ? "DIFF:" : "same", why ? why : "");
        diffs += why != NULL;
        free(name);
        if (units == cap) {
            cap = cap ? cap * 2 : 256;
            scripts = (char**)realloc(scripts, cap * sizeof *scripts);
            sizes = (uint32_t*)realloc(sizes, cap * sizeof *sizes);
            nodes = (char***)realloc(nodes, cap * sizeof *nodes);
            counts = (int*)realloc(counts, cap * sizeof *counts);
            if (!scripts || !sizes || !nodes || !counts) return 2;
        }
        scripts[units] = cob; sizes[units] = cn; nodes[units] = nm; counts[units] = (int)nn;
        units++;
    }
    for (u = 0; u < units; u++) {
        uint32_t len, i;
        char* c;
        for (len = 0; len < sizes[u]; len += len < 160 ? 1 : 13)
            probe(scripts[u], len, nodes[u], counts[u]);
        if (sizes[u] < 4) continue;
        c = (char*)malloc(sizes[u]);
        for (i = 0; i < 48; i++) {
            uint32_t at;
            memcpy(c, scripts[u], sizes[u]);
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            at = (seed % (sizes[u] / 4)) * 4;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            put_le(c + at, i & 1 ? seed : seed % 0x400u);
            probe(c, sizes[u], nodes[u], counts[u]);
        }
        free(c);
    }
    for (u = 0; u < units; u++) {
        int k;
        for (k = 0; k < counts[u]; k++) free(nodes[u][k]);
        free(nodes[u]);
        free(scripts[u]);
    }
    free(scripts); free(sizes); free(nodes); free(counts);
    fprintf(stderr, "cobscantest: %d scripts, %d relocated twins differing, fuzzed\n", units, diffs);
    return 0;
}
