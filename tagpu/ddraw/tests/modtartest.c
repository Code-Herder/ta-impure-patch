/* modtartest -- tagpu_modtar.c natively, under the sanitizers, by tools/modpack/installtest.py.

     modtartest TAR DECLARED [SEED]     prints "ok" or "refused: <why>", the tar fed in pieces chosen by SEED

   DECLARED holds one "<path>\t<size>\t<sha256 hex>" a line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_modtar.h"
#include "tagpu_modcat.h"

static int sink_begin(void* c, const char* p, uint32_t n) { (void)c; (void)p; (void)n; return 1; }
static int sink_data(void* c, const void* p, size_t n)
{
    unsigned long* sum = (unsigned long*)c;
    size_t i;
    for (i = 0; i < n; i++) *sum += ((const unsigned char*)p)[i];      /* touches every byte the reader hands over */
    return 1;
}
static int sink_end(void* c) { (void)c; return 1; }

static unsigned char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* b;
    long size;
    if (!f) exit(2);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char*)malloc((size_t)size + 1);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) exit(2);
    fclose(f);
    b[size] = 0;
    *n = (size_t)size;
    return b;
}

int main(int argc, char** argv)
{
    MODTAR t;
    MODTAR_SINK s;
    MODTAR_FILE files[64];
    unsigned char seen[64];
    unsigned long sum = 0;
    size_t ntar = 0, ndecl = 0, at = 0;
    unsigned char* tar;
    char* decl, *line;
    int nf = 0, ok = 1;

    if (argc < 3) return 2;
    tar = slurp(argv[1], &ntar);
    decl = (char*)slurp(argv[2], &ndecl);
    srand(argc > 3 ? (unsigned)atoi(argv[3]) : 1);
    for (line = strtok(decl, "\n"); line && nf < 64; line = strtok(NULL, "\n")) {
        char* a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!b) return 2;
        *a = 0;
        *b = 0;
        files[nf].path = line;
        files[nf].size = (uint32_t)strtoul(a + 1, NULL, 10);
        if (!tagpu_modcat_hash(b + 1, files[nf].sha256)) return 2;
        nf++;
    }
    s.begin = sink_begin;
    s.data = sink_data;
    s.end = sink_end;
    s.ctx = &sum;
    modtar_init(&t, &s, files, nf, seen);
    while (at < ntar && ok) {
        size_t piece = 1 + (size_t)rand() % 1500;
        if (piece > ntar - at) piece = ntar - at;
        {
            /* its own allocation, so the sanitizer sees a read past the piece */
            unsigned char* copy = (unsigned char*)malloc(piece);
            memcpy(copy, tar + at, piece);
            ok = modtar_feed(&t, copy, piece);
            free(copy);
        }
        at += piece;
    }
    if (ok) ok = modtar_finish(&t);
    printf(ok ? "ok\n" : "refused: %s\n", t.why);
    free(tar);
    free(decl);
    return ok ? 0 : 1;
}
