/* board_test.c -- the drawing board's operations and store (tagpu_boardwire.h, tagpu_boardstore.h)
   on the host: every operation's bytes there and back, every malformed body refused, the store's
   rights, eraser, ring, lifetime and replacement. board_test.py builds it with the sanitizers and
   parses the body it writes with recmp_file.board_ops. Exit 0 when every check holds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_boardwire.h"
#include "tagpu_boardstore.h"

static int s_fail;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); s_fail++; } } while (0)

static TagpuBoardOp mk(unsigned code, uint32_t id, int x, int h, int z, const char* text)
{
    TagpuBoardOp op;
    memset(&op, 0, sizeof op);
    op.op = code; op.id = id;
    op.a[0] = x; op.a[1] = h; op.a[2] = z;
    if (text) { op.len = (unsigned)strlen(text); memcpy(op.text, text, op.len); }
    return op;
}

static unsigned body_of(unsigned char* b, unsigned cap, unsigned seat, const TagpuBoardOp* ops, int n)
{
    unsigned at = TAGPU_BW_HEAD;
    int i;
    b[0] = TAGPU_BW_VERSION; b[1] = (unsigned char)seat;
    for (i = 0; i < n; i++) {
        unsigned k = tagpu_boardwire_put(b + at, cap - at, &ops[i], seat);
        CHECK(k);
        at += k;
    }
    return at;
}

static void wire(const char* out)
{
    unsigned char b[512], c[512];
    TagpuBoardOp ops[6], back;
    unsigned n, at, seat, i = 0;
    ops[0] = mk(TAGPU_BW_POINT, 3u << 24 | 7, 100, 20, -300, "Go here");
    ops[1] = mk(TAGPU_BW_EDIT, 1u << 24 | 2, 0, 0, 0, "edited ~");
    ops[2] = mk(TAGPU_BW_MOVE, 1u << 24 | 2, -TAGPU_BW_COORD, 0, TAGPU_BW_COORD, NULL);
    ops[3] = mk(TAGPU_BW_DELETE, 9u << 24 | 0xFFFFFF, 0, 0, 0, NULL);
    ops[4] = mk(TAGPU_BW_LINE, 0, 1, 2, 3, NULL); ops[4].b[0] = 4; ops[4].b[1] = 5; ops[4].b[2] = 6;
    ops[5] = mk(TAGPU_BW_ERASE, 0, 7, 8, 9, NULL);
    n = body_of(b, sizeof b, 3, ops, 6);
    CHECK(tagpu_boardwire_check(b, n, &seat) && seat == 3);
    for (at = TAGPU_BW_HEAD; at < n; i++) {
        unsigned next = tagpu_boardwire_next(b, n, at, &back);
        CHECK(next > at);
        if (next <= at) break;
        CHECK(back.op == ops[i].op && back.id == ops[i].id && back.len == ops[i].len &&
              !memcmp(back.a, ops[i].a, sizeof back.a) &&
              (back.op != TAGPU_BW_LINE || !memcmp(back.b, ops[i].b, sizeof back.b)) &&
              !strcmp(back.text, ops[i].text));
        at = next;
    }
    CHECK(i == 6);
    {
        /* renumbered: seat 3 is 0 here and seat 1 is 3; the body's seat, the POINT's id and the
           EDIT's and MOVE's owner follow, the sequences stay; a seat mapped to 10 refuses */
        unsigned char map[TAGPU_BW_SEATS] = { 5, 3, 2, 0, 4, 1, 6, 7, 8, 9 }, r[512];
        unsigned m = tagpu_boardwire_reseat(b, n, map, r, sizeof r), s2;
        TagpuBoardOp x;
        CHECK(m == n && tagpu_boardwire_check(r, m, &s2) && s2 == 0);
        CHECK(tagpu_boardwire_next(r, m, TAGPU_BW_HEAD, &x) && x.id == (0u << 24 | 7));
        at = tagpu_boardwire_next(r, m, TAGPU_BW_HEAD, &x);
        CHECK(tagpu_boardwire_next(r, m, at, &x) && x.id == (3u << 24 | 2));
        CHECK(!tagpu_boardwire_reseat(b, n, map, r, n - 1));
        map[9] = 10;                                         /* the DELETE names seat 9 */
        CHECK(!tagpu_boardwire_reseat(b, n, map, r, sizeof r));
    }
    if (out) { FILE* f = fopen(out, "wb"); CHECK(f); if (f) { fwrite(b, 1, n, f); fclose(f); } }

    /* refused: each truncation, the head, a POINT of another seat, a glyph, a coordinate, an id */
    for (i = 0; i < n; i++) {
        unsigned seat2;
        int ok = tagpu_boardwire_check(b, i, &seat2);
        int boundary = 0;
        for (at = TAGPU_BW_HEAD; at < n; ) { if (at == i) boundary = 1; at = tagpu_boardwire_next(b, n, at, &back); }
        CHECK(ok == (boundary && i > TAGPU_BW_HEAD));
    }
    memcpy(c, b, n); c[0] = 2; CHECK(!tagpu_boardwire_check(c, n, NULL));
    memcpy(c, b, n); c[1] = 10; CHECK(!tagpu_boardwire_check(c, n, NULL));
    memcpy(c, b, n); c[1] = 4; CHECK(!tagpu_boardwire_check(c, n, NULL));     /* the POINT is seat 3's */
    memcpy(c, b, n); c[2 + 18] = 0x7F; CHECK(!tagpu_boardwire_check(c, n, NULL));
    memcpy(c, b, n); c[2 + 1 + 4 + 3] = 0x7F; CHECK(!tagpu_boardwire_check(c, n, NULL));   /* x past the bound */
    memcpy(c, b, n); c[2 + 1 + 3] = 10; CHECK(!tagpu_boardwire_check(c, n, NULL));        /* id's seat 10 */
    memcpy(c, b, n); c[2] = 7; CHECK(!tagpu_boardwire_check(c, n, NULL));                 /* unknown code */
    {
        TagpuBoardOp bad = mk(TAGPU_BW_POINT, 3u << 24, 0, 0, 0, NULL);               /* sequence 0 */
        CHECK(!tagpu_boardwire_put(c, sizeof c, &bad, 3));
        bad = mk(TAGPU_BW_POINT, 3u << 24 | 1, 0, 0, 0, NULL); bad.len = 61;
        CHECK(!tagpu_boardwire_put(c, sizeof c, &bad, 3));
        bad = mk(TAGPU_BW_ERASE, 0, 0, 0, 0, NULL);
        CHECK(!tagpu_boardwire_put(c, 12, &bad, 3) && tagpu_boardwire_put(c, 13, &bad, 3) == 13);
    }
}

static unsigned live_segs(const TagpuBoardStore* s)
{
    unsigned i, n = 0;
    for (i = 0; i < s->nseg; i++) n += s->seg[(s->head + i) % TAGPU_BS_SEGS].live;
    return n;
}

static void store(void)
{
    static TagpuBoardStore s;
    TagpuBoardOp op;
    unsigned i;
    tagpu_boardstore_clear(&s, 100);
    op = mk(TAGPU_BW_POINT, 1u << 24 | 1, 0, 0, 0, "a");
    CHECK(tagpu_boardstore_apply(&s, 1, &op, 10, 0) == TAGPU_BS_NEW);
    CHECK(tagpu_boardstore_apply(&s, 1, &op, 11, 0) == TAGPU_BS_NONE);          /* the same id */
    CHECK(tagpu_boardstore_apply(&s, 2, &op, 11, 0) == TAGPU_BS_NONE);          /* not seat 2's */
    /* rights: seat 2 may not move seat 1's marker without seat 1 in its mask, and may with it */
    op = mk(TAGPU_BW_MOVE, 1u << 24 | 1, 50, 0, 50, NULL);
    CHECK(tagpu_boardstore_apply(&s, 2, &op, 12, 0) == TAGPU_BS_NONE);
    CHECK(tagpu_boardstore_apply(&s, 2, &op, 12, 1u << 1) == TAGPU_BS_CHANGED);
    CHECK(tagpu_boardstore_find(&s, 1u << 24 | 1)->x == 50);
    op = mk(TAGPU_BW_EDIT, 1u << 24 | 1, 0, 0, 0, "bb");
    CHECK(tagpu_boardstore_apply(&s, 1, &op, 12, 0) == TAGPU_BS_CHANGED &&
          !strcmp(tagpu_boardstore_find(&s, 1u << 24 | 1)->text, "bb"));
    /* the eraser: its radius, and only the owners it may touch */
    op = mk(TAGPU_BW_LINE, 0, 0, 0, 0, NULL); op.b[0] = 200;
    CHECK(tagpu_boardstore_apply(&s, 3, &op, 13, 0) == TAGPU_BS_CHANGED);
    op = mk(TAGPU_BW_ERASE, 0, 100, 0, 49, NULL);
    CHECK(tagpu_boardstore_apply(&s, 2, &op, 14, 0) == TAGPU_BS_NONE);          /* 49 away, not 2's */
    CHECK(tagpu_boardstore_apply(&s, 3, &op, 14, 0) == TAGPU_BS_NONE);          /* 49 > 48 */
    op = mk(TAGPU_BW_ERASE, 0, 100, 0, 47, NULL);
    CHECK(tagpu_boardstore_apply(&s, 3, &op, 14, 0) == TAGPU_BS_CHANGED && live_segs(&s) == 0);
    op = mk(TAGPU_BW_ERASE, 0, 60, 0, 60, NULL);
    CHECK(tagpu_boardstore_apply(&s, 2, &op, 14, 0) == TAGPU_BS_NONE);
    CHECK(tagpu_boardstore_apply(&s, 2, &op, 14, 1u << 1) == TAGPU_BS_CHANGED &&
          !tagpu_boardstore_find(&s, 1u << 24 | 1));
    /* the ring keeps the newest TAGPU_BS_SEGS */
    for (i = 0; i < TAGPU_BS_SEGS + 10; i++) {
        op = mk(TAGPU_BW_LINE, 0, (int)i, 0, 0, NULL);
        tagpu_boardstore_apply(&s, 0, &op, 20, 0);
    }
    CHECK(s.nseg == TAGPU_BS_SEGS && s.seg[s.head].a[0] == 10);   /* the erased one went first */
    /* the lifetime: gone at 100 ticks old, kept by a tick that went back */
    tagpu_boardstore_clear(&s, 100);
    op = mk(TAGPU_BW_POINT, 0u << 24 | 1, 0, 0, 0, NULL); tagpu_boardstore_apply(&s, 0, &op, 1000, 0);
    op = mk(TAGPU_BW_LINE, 0, 0, 0, 0, NULL); tagpu_boardstore_apply(&s, 0, &op, 1050, 0);
    tagpu_boardstore_expire(&s, 900);
    CHECK(tagpu_boardstore_find(&s, 1) && live_segs(&s) == 1);
    tagpu_boardstore_expire(&s, 1099);
    CHECK(tagpu_boardstore_find(&s, 1) && live_segs(&s) == 1);
    tagpu_boardstore_expire(&s, 1100);
    CHECK(!tagpu_boardstore_find(&s, 1) && live_segs(&s) == 1);
    tagpu_boardstore_expire(&s, 1150);
    CHECK(live_segs(&s) == 0 && s.nseg == 0);
    /* until erased: a life of 0 keeps everything */
    tagpu_boardstore_clear(&s, 0);
    op = mk(TAGPU_BW_POINT, 0u << 24 | 1, 0, 0, 0, NULL); tagpu_boardstore_apply(&s, 0, &op, 0, 0);
    tagpu_boardstore_expire(&s, 0x7FFFFFFF);
    CHECK(tagpu_boardstore_find(&s, 1));
    /* every slot taken: the oldest is replaced */
    tagpu_boardstore_clear(&s, 0);
    for (i = 0; i < TAGPU_BS_MARKS; i++) {
        op = mk(TAGPU_BW_POINT, 5u << 24 | (i + 1), 0, 0, 0, NULL);
        CHECK(tagpu_boardstore_apply(&s, 5, &op, i == 7 ? 1 : 100 + i, 0) == TAGPU_BS_NEW);
    }
    op = mk(TAGPU_BW_POINT, 5u << 24 | 999, 0, 0, 0, NULL);
    CHECK(tagpu_boardstore_apply(&s, 5, &op, 1000, 0) == TAGPU_BS_NEW);
    CHECK(!tagpu_boardstore_find(&s, 5u << 24 | 8) && tagpu_boardstore_find(&s, 5u << 24 | 999));
}

int main(int argc, char** argv)
{
    wire(argc > 1 ? argv[1] : NULL);
    store();
    printf(s_fail ? "board_test: %d FAILED\n" : "board_test: PASS%.0d\n", s_fail);
    return s_fail != 0;
}
