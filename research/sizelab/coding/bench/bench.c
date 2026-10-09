/* bench.c -- the writer's compressor, measured as the DLL would run it: 32-bit MinGW, -O2,
   the default target (pentiumpro, no SSE2), on the recording's own stream blocks.

   bench IDX BIN CODEC LEVEL [MODE [DICT]]
     CODEC  lodepng (LEVEL = window size), zstd, lz4, lz4hc, libdeflate
     MODE   block  (default) each stream block alone, as the file is today
            chain  zstd only: one streaming frame from a snapshot to the next, flushed at each
                   block's end (ZSTD_e_flush), so a block is the bytes the flush emitted
            dict   zstd only: each block alone with the dictionary file DICT
   Prints packed bytes, compress and decompress seconds, the slowest block, the busiest 30 s of
   GameTime's compress seconds, and checks every block round-trips. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lodepng.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "lz4.h"
#include "lz4hc.h"
#include "libdeflate.h"
#include <brotli/encode.h>
#include <brotli/decode.h>

typedef struct { unsigned kind, first, last, raw, packed, off; } Idx;

static double now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static unsigned char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* p;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(*n ? *n : 1);
    if (fread(p, 1, *n, f) != *n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    return p;
}

int main(int argc, char** argv)
{
    size_t in, bn, dn = 0;
    unsigned char* ib;
    unsigned char* bin;
    unsigned char* dict = NULL;
    Idx* ix;
    unsigned nb, i;
    const char* codec;
    int level;
    const char* mode = "block";
    double tc = 0, td = 0, slow = 0, win[64] = {0};
    unsigned long long raw = 0, packed = 0;
    unsigned char* out;
    unsigned char* back;
    size_t cap = 64u << 20;
    ZSTD_CCtx* cc = NULL;
    ZSTD_DCtx* dc = NULL;
    ZSTD_CDict* cd = NULL;
    ZSTD_DDict* dd = NULL;
    struct libdeflate_compressor* lc = NULL;
    struct libdeflate_decompressor* ld = NULL;
    unsigned char** pk;
    size_t* pn;
    unsigned* pchain;    /* the chain a block belongs to (chain mode) */
    unsigned chain = 0;

    if (argc < 5) { fprintf(stderr, "usage: bench IDX BIN CODEC LEVEL [MODE [DICT]]\n"); return 2; }
    ib = slurp(argv[1], &in);
    bin = slurp(argv[2], &bn);
    codec = argv[3];
    level = atoi(argv[4]);
    if (argc > 5) mode = argv[5];
    if (argc > 6) dict = slurp(argv[6], &dn);
    memcpy(&nb, ib, 4);
    ix = (Idx*)(ib + 4);
    out = malloc(cap);
    back = malloc(cap);
    pk = calloc(nb, sizeof *pk);
    pn = calloc(nb, sizeof *pn);
    pchain = calloc(nb, sizeof *pchain);

    if (!strcmp(codec, "zstd")) {
        cc = ZSTD_createCCtx();
        dc = ZSTD_createDCtx();
        ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, level);
        ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 0);
        ZSTD_CCtx_setParameter(cc, ZSTD_c_contentSizeFlag, strcmp(mode, "chain") ? 1 : 0);
        if (getenv("WLOG")) ZSTD_CCtx_setParameter(cc, ZSTD_c_windowLog, atoi(getenv("WLOG")));
        if (getenv("LDM")) ZSTD_CCtx_setParameter(cc, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 27);
        if (dict) {
            cd = ZSTD_createCDict(dict, dn, level);
            dd = ZSTD_createDDict(dict, dn);
        }
    } else if (!strcmp(codec, "libdeflate")) {
        lc = libdeflate_alloc_compressor(level);
        ld = libdeflate_alloc_decompressor();
    }

    /* compress: the stream blocks in file order; a snapshot block ends a chain */
    for (i = 0; i < nb; i++) {
        const unsigned char* p = bin + ix[i].off;
        size_t n = ix[i].raw, z = 0;
        double t0;
        if (ix[i].kind == 3) { chain++; if (cc && !strcmp(mode, "chain")) ZSTD_CCtx_reset(cc, ZSTD_reset_session_only); continue; }
        if (ix[i].kind != 2) continue;
        t0 = now();
        if (!strcmp(codec, "lodepng")) {
            LodePNGCompressSettings s = lodepng_default_compress_settings;
            unsigned char* zz = NULL;
            size_t zn = 0;
            s.windowsize = (unsigned)level;
            if (getenv("NICE")) s.nicematch = atoi(getenv("NICE"));
            if (lodepng_zlib_compress(&zz, &zn, p, n, &s)) { fprintf(stderr, "lodepng failed\n"); return 1; }
            memcpy(out, zz, zn);
            z = zn;
            free(zz);
        } else if (!strcmp(codec, "zstd")) {
            if (!strcmp(mode, "chain")) {
                ZSTD_inBuffer ib2 = { p, n, 0 };
                ZSTD_outBuffer ob = { out, cap, 0 };
                size_t r;
                do {
                    r = ZSTD_compressStream2(cc, &ob, &ib2, ZSTD_e_flush);
                    if (ZSTD_isError(r)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(r)); return 1; }
                } while (r);
                z = ob.pos;
            } else if (cd) {
                z = ZSTD_compress_usingCDict(cc, out, cap, p, n, cd);
            } else {
                z = ZSTD_compress2(cc, out, cap, p, n);
            }
            if (ZSTD_isError(z)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(z)); return 1; }
        } else if (!strcmp(codec, "lz4")) {
            z = (size_t)LZ4_compress_fast((const char*)p, (char*)out, (int)n, (int)cap, level ? level : 1);
        } else if (!strcmp(codec, "lz4hc")) {
            z = (size_t)LZ4_compress_HC((const char*)p, (char*)out, (int)n, (int)cap, level);
        } else if (!strcmp(codec, "libdeflate")) {
            z = libdeflate_zlib_compress(lc, p, n, out, cap);
        } else if (!strcmp(codec, "brotli")) {
            size_t zn = cap;
            if (!BrotliEncoderCompress(level, 22, BROTLI_MODE_GENERIC, n, p, &zn, out)) { fprintf(stderr, "brotli failed\n"); return 1; }
            z = zn;
        }
        {
            double t = now() - t0;
            tc += t;
            if (t > slow) slow = t;
            if (ix[i].first / 900 < 64) win[ix[i].first / 900] += t;
        }
        pk[i] = malloc(z ? z : 1);
        memcpy(pk[i], out, z);
        pn[i] = z;
        pchain[i] = chain;
        raw += n;
        packed += z;
    }

    /* decompress and check, in the same order */
    if (dc && !strcmp(mode, "chain")) ZSTD_DCtx_reset(dc, ZSTD_reset_session_only);
    chain = 0;
    for (i = 0; i < nb; i++) {
        const unsigned char* p = bin + ix[i].off;
        size_t n = ix[i].raw, got = 0;
        double t0;
        if (ix[i].kind == 3) { chain++; if (dc && !strcmp(mode, "chain")) ZSTD_DCtx_reset(dc, ZSTD_reset_session_only); continue; }
        if (ix[i].kind != 2) continue;
        t0 = now();
        if (!strcmp(codec, "lodepng")) {
            unsigned char* d = NULL;
            size_t dn2 = 0;
            if (lodepng_zlib_decompress(&d, &dn2, pk[i], pn[i], &lodepng_default_decompress_settings)) { fprintf(stderr, "inflate failed\n"); return 1; }
            memcpy(back, d, dn2);
            got = dn2;
            free(d);
        } else if (!strcmp(codec, "zstd")) {
            if (!strcmp(mode, "chain")) {
                ZSTD_inBuffer ib2 = { pk[i], pn[i], 0 };
                ZSTD_outBuffer ob = { back, cap, 0 };
                while (ib2.pos < ib2.size) {
                    size_t r = ZSTD_decompressStream(dc, &ob, &ib2);
                    if (ZSTD_isError(r)) { fprintf(stderr, "zstd d: %s\n", ZSTD_getErrorName(r)); return 1; }
                }
                got = ob.pos;
            } else if (dd) {
                got = ZSTD_decompress_usingDDict(dc, back, cap, pk[i], pn[i], dd);
            } else {
                got = ZSTD_decompressDCtx(dc, back, cap, pk[i], pn[i]);
            }
        } else if (!strcmp(codec, "lz4") || !strcmp(codec, "lz4hc")) {
            got = (size_t)LZ4_decompress_safe((const char*)pk[i], (char*)back, (int)pn[i], (int)cap);
        } else if (!strcmp(codec, "libdeflate")) {
            if (libdeflate_zlib_decompress(ld, pk[i], pn[i], back, n, &got)) { fprintf(stderr, "libdeflate d failed\n"); return 1; }
        } else if (!strcmp(codec, "brotli")) {
            size_t dn3 = cap;
            if (BrotliDecoderDecompress(pn[i], pk[i], &dn3, back) != BROTLI_DECODER_RESULT_SUCCESS) { fprintf(stderr, "brotli d failed\n"); return 1; }
            got = dn3;
        }
        td += now() - t0;
        if (got != n || memcmp(back, p, n)) { fprintf(stderr, "block %u does not round-trip (%u vs %u)\n", i, (unsigned)got, (unsigned)n); return 1; }
    }
    {
        double busiest = 0;
        for (i = 0; i < 64; i++) if (win[i] > busiest) busiest = win[i];
        printf("%s %d %s raw %.1f MB packed %.3f MB  comp %.2f s (%.1f MB/s) dec %.3f s (%.0f MB/s)  slowest block %.1f ms  busiest 30s %.2f s  dict %u B",
               codec, level, mode, raw / 1e6, packed / 1e6, tc, raw / 1e6 / tc, td, raw / 1e6 / td, slow * 1e3, busiest, (unsigned)dn);
        if (cc) {
            ZSTD_compressionParameters cp = ZSTD_getCParams(level, 0, 0);
            printf("  wlog %u cctx est %.1f MB cstream est %.1f MB dstream est %.1f MB", cp.windowLog,
                   ZSTD_estimateCCtxSize_usingCParams(ZSTD_getCParams(level, 300000, 0)) / 1e6,
                   ZSTD_estimateCStreamSize_usingCParams(cp) / 1e6,
                   ZSTD_estimateDStreamSize((size_t)1 << cp.windowLog) / 1e6);
        }
        printf("\n");
    }
    return 0;
}
