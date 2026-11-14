/* The skippable before-call stub's bytes (tagpu/ddraw/inc/tagpu_skipstub.h), compiled as the DLL
   compiles them and run natively by skipstub_test.py.

   WHAT IT HOLDS TO WHAT.
   (a) The literal: the stub tagpu_replay.c's NO DRAW puts at the frame visit 0x4969CD -- `fn` the
       visit, `skip` its flag, `engine` DrawGameScreen 0x468CF0 and its own `ret 8` -- written
       out by hand for a stub at 0x10000000, byte for byte.
   (b) Every rel32 and the jne decoded back over random placements: the call reaches `fn`, the jmp
       reaches `engine`, the jne lands on the `ret`, and the stub is TAGPU_SKIPSTUB_BYTES long.

     skipstub_test [seed] [cases] */
#include "tagpu_skipstub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned s_rng;
static unsigned rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }

static unsigned rel_at(const unsigned char* p, unsigned at, unsigned off)
{
    unsigned rel = (unsigned)p[off] | (unsigned)p[off + 1] << 8 | (unsigned)p[off + 2] << 16 |
                   (unsigned)p[off + 3] << 24;
    return at + off + 4 + rel;
}

int main(int argc, char** argv)
{
    static const unsigned char WANT[TAGPU_SKIPSTUB_BYTES] = {
        0x9C, 0x60,                                     /* pushfd; pushad          */
        0xE8, 0xF9, 0x0F, 0x00, 0x00,                   /* call 0x10001000         */
        0x61, 0x9D,                                     /* popad; popfd            */
        0x80, 0x3D, 0x00, 0x20, 0x00, 0x10, 0x00,       /* cmp byte [0x10002000],0 */
        0x75, 0x05,                                     /* jne +5                  */
        0xE9, 0xD9, 0x8C, 0x46, 0xF0,                   /* jmp 0x468CF0            */
        0xC2, 0x08, 0x00                                /* ret 8                   */
    };
    unsigned char got[TAGPU_SKIPSTUB_BYTES + 8];
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 10000u;
    unsigned i, n;

    memset(got, 0xCC, sizeof got);
    n = tagpu_skipstub_emit(got, 0x10000000u, 0x10001000u, 0x10002000u, 0x00468CF0u, 8);
    CHECK(n == TAGPU_SKIPSTUB_BYTES);
    CHECK(!memcmp(got, WANT, sizeof WANT));
    CHECK(got[TAGPU_SKIPSTUB_BYTES] == 0xCC);
    printf("the frame visit's stub: %u bytes, as written out\n", n);

    s_rng = seed;
    for (i = 0; i < cases; i++) {
        const unsigned at = rnd() & ~0xFu, fn = rnd(), skip = rnd(), engine = rnd();
        const unsigned short retn = (unsigned short)(rnd() & 0xFFFC);
        unsigned flag;
        memset(got, 0xCC, sizeof got);
        n = tagpu_skipstub_emit(got, at, fn, skip, engine, retn);
        CHECK(n == TAGPU_SKIPSTUB_BYTES);
        CHECK(got[0] == 0x9C && got[1] == 0x60 && got[2] == 0xE8);
        CHECK(rel_at(got, at, 3) == fn);
        CHECK(got[7] == 0x61 && got[8] == 0x9D && got[9] == 0x80 && got[10] == 0x3D);
        flag = (unsigned)got[11] | (unsigned)got[12] << 8 | (unsigned)got[13] << 16 |
               (unsigned)got[14] << 24;
        CHECK(flag == skip && got[15] == 0x00);
        CHECK(got[16] == 0x75 && 18 + got[17] == 23);     /* the jne lands on the ret */
        CHECK(got[18] == 0xE9 && rel_at(got, at, 19) == engine);
        CHECK(got[23] == 0xC2 && (got[24] | got[25] << 8) == retn);
        CHECK(got[TAGPU_SKIPSTUB_BYTES] == 0xCC);
    }
    printf("%u random placements decode to their targets\n", cases);
    printf("every check holds\n");
    return 0;
}
