/* The skippable before-call stub's and the skippable call's bytes (tagpu/ddraw/inc/
   tagpu_skipstub.h), compiled as the DLL compiles them and run natively by skipstub_test.py.

   WHAT IT HOLDS TO WHAT.
   (a) The literal: the stub tagpu_replay.c's NO DRAW puts at the frame visit 0x4969CD -- `fn` the
       visit, `skip` its flag, `engine` DrawGameScreen 0x468CF0 and `alt` the undrawn frame -- written
       out by hand for a stub at 0x10000000, byte for byte.
   (b) Every rel32 and the jne decoded back over random placements: the call reaches `fn`, the
       first jmp `engine`, the second `alt`, the jne lands on the second jmp, and the stub is
       TAGPU_SKIPSTUB_BYTES long.
   (c) The same two for the gate NO DRAW puts at DrawUnit's composite call 0x45AE73 -- `skip` its
       flag, `engine` the composite 0x458810 and its `ret 8` -- and its random placements: the jmp
       reaches `engine`, the jne lands on the `ret`, and the gate is TAGPU_SKIPCALL_BYTES long.

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
        0xE9, 0xE4, 0x2F, 0x00, 0x00                    /* jmp 0x10003000          */
    };
    static const unsigned char WANT_CALL[TAGPU_SKIPCALL_BYTES] = {
        0x80, 0x3D, 0x00, 0x20, 0x00, 0x10, 0x00,       /* cmp byte [0x10002000],0 */
        0x75, 0x05,                                     /* jne +5                  */
        0xE9, 0x02, 0x88, 0x45, 0xF0,                   /* jmp 0x458810            */
        0xC2, 0x08, 0x00                                /* ret 8                   */
    };
    unsigned char got[TAGPU_SKIPSTUB_BYTES + 8];
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 10000u;
    unsigned i, n;

    memset(got, 0xCC, sizeof got);
    n = tagpu_skipstub_emit(got, 0x10000000u, 0x10001000u, 0x10002000u, 0x00468CF0u, 0x10003000u);
    CHECK(n == TAGPU_SKIPSTUB_BYTES);
    CHECK(!memcmp(got, WANT, sizeof WANT));
    CHECK(got[TAGPU_SKIPSTUB_BYTES] == 0xCC);
    printf("the frame visit's stub: %u bytes, as written out\n", n);

    s_rng = seed;
    for (i = 0; i < cases; i++) {
        const unsigned at = rnd() & ~0xFu, fn = rnd(), skip = rnd(), engine = rnd(), alt = rnd();
        unsigned flag;
        memset(got, 0xCC, sizeof got);
        n = tagpu_skipstub_emit(got, at, fn, skip, engine, alt);
        CHECK(n == TAGPU_SKIPSTUB_BYTES);
        CHECK(got[0] == 0x9C && got[1] == 0x60 && got[2] == 0xE8);
        CHECK(rel_at(got, at, 3) == fn);
        CHECK(got[7] == 0x61 && got[8] == 0x9D && got[9] == 0x80 && got[10] == 0x3D);
        flag = (unsigned)got[11] | (unsigned)got[12] << 8 | (unsigned)got[13] << 16 |
               (unsigned)got[14] << 24;
        CHECK(flag == skip && got[15] == 0x00);
        CHECK(got[16] == 0x75 && 18 + got[17] == 23);     /* the jne lands on the second jmp */
        CHECK(got[18] == 0xE9 && rel_at(got, at, 19) == engine);
        CHECK(got[23] == 0xE9 && rel_at(got, at, 24) == alt);
        CHECK(got[TAGPU_SKIPSTUB_BYTES] == 0xCC);
    }
    printf("%u random placements decode to their targets\n", cases);

    memset(got, 0xCC, sizeof got);
    n = tagpu_skipcall_emit(got, 0x10000000u, 0x10002000u, 0x00458810u, 8);
    CHECK(n == TAGPU_SKIPCALL_BYTES);
    CHECK(!memcmp(got, WANT_CALL, sizeof WANT_CALL));
    CHECK(got[TAGPU_SKIPCALL_BYTES] == 0xCC);
    printf("the composite's gate: %u bytes, as written out\n", n);
    for (i = 0; i < cases; i++) {
        const unsigned at = rnd() & ~0xFu, skip = rnd(), engine = rnd();
        const unsigned short retn = (unsigned short)(rnd() & 0xFFFC);
        unsigned flag;
        memset(got, 0xCC, sizeof got);
        n = tagpu_skipcall_emit(got, at, skip, engine, retn);
        CHECK(n == TAGPU_SKIPCALL_BYTES);
        CHECK(got[0] == 0x80 && got[1] == 0x3D);
        flag = (unsigned)got[2] | (unsigned)got[3] << 8 | (unsigned)got[4] << 16 |
               (unsigned)got[5] << 24;
        CHECK(flag == skip && got[6] == 0x00);
        CHECK(got[7] == 0x75 && 9 + got[8] == 14);         /* the jne lands on the ret */
        CHECK(got[9] == 0xE9 && rel_at(got, at, 10) == engine);
        CHECK(got[14] == 0xC2 && (got[15] | got[16] << 8) == retn);
        CHECK(got[TAGPU_SKIPCALL_BYTES] == 0xCC);
    }
    printf("%u random placements of the gate decode to their targets\n", cases);
    printf("every check holds\n");
    return 0;
}
