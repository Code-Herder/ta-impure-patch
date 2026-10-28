/* wavtest.c -- tagpu_wav.c on hostile input, built natively with the sanitizers by tests/wavtest.py.

   wavtest            the built-in cases: each a file made here and the verdict it must get;
                      prints one line a case, exits 1 when one differs
   wavtest FILE...    each file's verdict, one line each: `ok <channels> <bits> <rate> <bytes>` or
                      `refused <why>`, for the driver to compare with tools/wiki/gfx.py `wav_pcm`;
                      every truncation of each file is parsed too, under the sanitizers, and a
                      verdict that reads past the bytes given is a failure */
#include "tagpu_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* a file of `riff` RIFF size, a fmt chunk of `fmtlen` (tag, channels, rate, align, bits) and a data
   chunk claiming `datalen` bytes, holding `have` of them; `n` is the length written */
static size_t make(unsigned char* b, unsigned riff, unsigned fmtlen, unsigned tag, unsigned ch, unsigned rate,
                   unsigned align, unsigned bits, unsigned datalen, unsigned have)
{
    size_t n = 0;
    memcpy(b, "RIFF", 4);
    put32(b + 4, riff);
    memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4);
    put32(b + 16, fmtlen);
    put16(b + 20, tag);
    put16(b + 22, ch);
    put32(b + 24, rate);
    put32(b + 28, rate * align);
    put16(b + 32, align);
    put16(b + 34, bits);
    n = 36;
    memcpy(b + n, "data", 4);
    put32(b + n + 4, datalen);
    n += 8;
    memset(b + n, 0x80, have);
    return n + have;
}

static int failed;
static void expect(const char* what, const unsigned char* b, size_t n, int ok, unsigned bytes)
{
    TAGPU_WAV w;
    char why[96] = "";
    /* a copy exactly `n` long on the heap, so the sanitizer sees a read past it */
    unsigned char* c = (unsigned char*)malloc(n ? n : 1);
    int got;
    if (n) memcpy(c, b, n);
    got = tagpu_wav_parse(c, n, &w, why, sizeof why);
    if (got != ok || (ok && w.bytes != bytes) || (ok && (w.data < c || w.data + w.bytes > c + n))) {
        failed++;
        printf("FAIL %s: %s %s (bytes %u, want %s %u)\n", what, got ? "ok" : "refused", why, got ? w.bytes : 0, ok ? "ok" : "refused", bytes);
    } else
        printf("ok   %s: %s%s\n", what, got ? "accepted" : "refused - ", got ? "" : why);
    free(c);
}

static int builtin(void)
{
    static unsigned char b[4096];
    size_t n, i;
    n = make(b, 36 + 100, 16, 1, 1, 22050, 1, 8, 100, 100);
    expect("8-bit mono PCM", b, n, 1, 100);
    n = make(b, 36 + 100, 16, 1, 2, 44100, 4, 16, 100, 100);
    expect("16-bit stereo PCM, data cut to whole frames", b, n, 1, 100);
    n = make(b, 36 + 102, 16, 1, 2, 44100, 4, 16, 102, 102);
    expect("16-bit stereo, a partial last frame", b, n, 1, 100);
    n = make(b, 36 + 100, 16, 2, 1, 22050, 256, 4, 100, 100);
    expect("ADPCM (tag 2)", b, n, 0, 0);
    n = make(b, 36 + 100, 16, 3, 1, 22050, 4, 32, 100, 100);
    expect("IEEE float (tag 3)", b, n, 0, 0);
    n = make(b, 36 + 100, 16, 1, 1, 22050, 2, 8, 100, 100);
    expect("a block align that is not the frame", b, n, 0, 0);
    n = make(b, 36 + 100, 16, 1, 3, 22050, 3, 8, 100, 100);
    expect("three channels", b, n, 0, 0);
    n = make(b, 36 + 100, 16, 1, 1, 0xFFFFFFFFu, 1, 8, 100, 100);
    expect("a rate of 2^32 - 1", b, n, 0, 0);
    n = make(b, 0xFFFFFFFFu, 16, 1, 1, 22050, 1, 8, 0xFFFFFFF0u, 100);
    expect("huge RIFF and data sizes: the data cut to the file", b, n, 1, 100);
    n = make(b, 36 + 100, 0xFFFFFFF0u, 1, 1, 22050, 1, 8, 100, 100);
    expect("a huge fmt length: the walk ends there, no data", b, n, 0, 0);
    n = make(b, 36 + 100, 8, 1, 1, 22050, 1, 8, 100, 100);
    expect("a fmt chunk of 8 bytes", b, n, 0, 0);
    n = make(b, 36 + 100, 16, 1, 1, 22050, 1, 8, 100, 0);
    expect("data claimed, none present", b, n, 0, 0);
    n = make(b, 20, 16, 1, 1, 22050, 1, 8, 100, 100);
    expect("a RIFF size that ends inside fmt", b, n, 0, 0);
    memcpy(b + 12, "data", 4);
    expect("data before fmt", b, n, 0, 0);
    n = make(b, 36 + 100, 16, 1, 1, 22050, 1, 8, 100, 100);
    memcpy(b, "RIFX", 4);
    expect("not RIFF", b, n, 0, 0);
    expect("empty", b, 0, 0, 0);
    /* every truncation of a good file: none may read past its end */
    n = make(b, 36 + 100, 16, 1, 1, 22050, 1, 8, 100, 100);
    for (i = 0; i < n; i++) {
        TAGPU_WAV w;
        unsigned char* c = (unsigned char*)malloc(i ? i : 1);
        if (i) memcpy(c, b, i);
        if (tagpu_wav_parse(c, i, &w, NULL, 0) && w.data + w.bytes > c + i) {
            failed++;
            printf("FAIL truncation at %u: data past the end\n", (unsigned)i);
        }
        free(c);
    }
    printf("%s: %d failed\n", failed ? "FAILED" : "passed", failed);
    return failed ? 1 : 0;
}

int main(int argc, char** argv)
{
    int a;
    if (argc < 2) return builtin();
    for (a = 1; a < argc; a++) {
        FILE* f = fopen(argv[a], "rb");
        unsigned char* b;
        long n;
        size_t i;
        TAGPU_WAV w;
        char why[96] = "";
        if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
            printf("unreadable\n");
            if (f) fclose(f);
            continue;
        }
        b = (unsigned char*)malloc(n ? (size_t)n : 1);
        if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
            printf("unreadable\n");
            free(b);
            fclose(f);
            continue;
        }
        fclose(f);
        if (tagpu_wav_parse(b, (size_t)n, &w, why, sizeof why))
            printf("ok %d %d %d %u\n", w.channels, w.bits, w.rate, (unsigned)w.bytes);
        else
            printf("refused %s\n", why);
        /* each truncation on the heap at its own length, for the sanitizer */
        for (i = 0; i < (size_t)n && i < 256; i++) {
            unsigned char* c = (unsigned char*)malloc(i ? i : 1);
            if (i) memcpy(c, b, i);
            if (tagpu_wav_parse(c, i, &w, NULL, 0) && w.data + w.bytes > c + i) {
                failed++;
                printf("FAIL %s truncated to %u: data past the end\n", argv[a], (unsigned)i);
            }
            free(c);
        }
        free(b);
    }
    return failed ? 1 : 0;
}
