#include "tagpu_recmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned applied_tick;

static int put(RecMpOrderCache* c, RecMpHeader* h, unsigned slot, unsigned list,
               unsigned birth, unsigned total, unsigned first, unsigned count, unsigned char value)
{
    unsigned char body[RECMP_BODY_MAX];
    RecMpOrders chunk = { .slot = slot, .list = list, .birth = birth, .total = total,
                          .first = first, .count = count };
    unsigned bytes = sizeof chunk + count * RECMP_ORDER_BYTES;
    CHECK(bytes <= sizeof body);
    memcpy(body, &chunk, sizeof chunk);
    memset(body + sizeof chunk, value, bytes - sizeof chunk);
    return tagpu_recmp_orders_put(c, h, body, bytes, applied_tick);
}

int main(void)
{
    RecMpOrderCache* c = tagpu_recmp_orders_begin(100);
    RecMpHeader h = { .origin = 11, .tick = 9, .sequence = 1, .stream = { 1 } };
    RecMpHeader ai = { .origin = 22, .tick = 10, .sequence = 1, .stream = { 2 } };
    RecMpOrderState state;
    unsigned char image[RECMP_ORDER_MAX * RECMP_ORDER_BYTES];
    unsigned i, first;
    CHECK(c && sizeof(RecMpOrders) == 16 && RECMP_ORDER_CHUNK == 45);
    CHECK(!tagpu_recmp_orders_idle(NULL,11) && !tagpu_recmp_orders_idle(c,0));
    CHECK(tagpu_recmp_orders_idle(c,11));
    CHECK(!tagpu_recmp_orders_begin(65537));
    applied_tick = 8;
    CHECK(put(c, &h, 1, 0, 3, 1, 0, 1, 0x11) == 1);
    CHECK(tagpu_recmp_orders_get(c, 1, 0, &state, image, 1));
    CHECK(state.origin == 11 && state.birth == 3 && state.source_tick == 9 && state.sequence == 1);
    CHECK(state.applied_tick == 8);
    for (i = 0; i < RECMP_ORDER_BYTES; i++) CHECK(image[i] == 0x11);
    CHECK(tagpu_recmp_orders_same(c, 1, 0, 11, 3, image, 1));
    CHECK(!tagpu_recmp_orders_same(c, 1, 0, 11, 4, image, 1));
    CHECK(!tagpu_recmp_orders_same(c, 1, 0, 22, 3, image, 1));

    h.sequence++; h.tick++;
    applied_tick = 11;
    CHECK(put(c, &h, 1, 0, 3, 46, 0, 45, 0x22) == 0);
    CHECK(!tagpu_recmp_orders_idle(c,11) && tagpu_recmp_orders_idle(c,22));
    CHECK(tagpu_recmp_orders_get(c, 1, 0, &state, image, 1) && image[0] == 0x11);
    CHECK(state.source_tick == 9 && state.applied_tick == 8 && state.sequence == 1);
    CHECK(put(c, &ai, 2, 1, 4, 2, 0, 2, 0x33) == 1);
    CHECK(!tagpu_recmp_orders_idle(c,11) && tagpu_recmp_orders_idle(c,22));
    CHECK(tagpu_recmp_orders_get(c, 2, 1, &state, image, 2));
    CHECK(state.source_tick == 10 && state.applied_tick == 11);
    CHECK(put(c, &h, 1, 0, 3, 46, 0, 45, 0x44) == -1); /* duplicate */
    h.sequence += 2;
    CHECK(put(c, &h, 1, 0, 3, 46, 45, 1, 0x44) == -1); /* missing sequence */
    h.sequence--;
    CHECK(put(c, &h, 1, 0, 4, 46, 45, 1, 0x44) == -1); /* another incarnation */
    CHECK(tagpu_recmp_orders_get(c, 1, 0, &state, image, 1));
    CHECK(state.source_tick == 9 && state.applied_tick == 8 && state.sequence == 1);
    applied_tick = 15;
    CHECK(put(c, &h, 1, 0, 3, 46, 45, 1, 0x44) == 1);
    CHECK(tagpu_recmp_orders_idle(c,11));
    CHECK(!tagpu_recmp_orders_get(c, 1, 0, &state, image, 45));
    CHECK(tagpu_recmp_orders_get(c, 1, 0, &state, image, 46) && state.count == 46);
    CHECK(state.source_tick == 10 && state.applied_tick == 15 && state.sequence == 3);
    for (i = 0; i < 45 * RECMP_ORDER_BYTES; i++) CHECK(image[i] == 0x22);
    for (; i < 46 * RECMP_ORDER_BYTES; i++) CHECK(image[i] == 0x44);
    h.sequence++;
    CHECK(put(c, &h, 1, 0, 3, 0, 0, 0, 0) == 1);
    CHECK(tagpu_recmp_orders_get(c, 1, 0, &state, NULL, 0) && !state.count);
    CHECK(put(c, &h, 1, 0, 3, 1, 0, 1, 0) == -1); /* stale completed transaction */
    h.sequence++;
    CHECK(put(c, &h, 100, 0, 3, 1, 0, 1, 0) == -1);
    CHECK(put(c, &h, 1, 2, 3, 1, 0, 1, 0) == -1);
    CHECK(put(c, &h, 1, 0, 3, 4097, 0, 1, 0) == -1);
    CHECK(put(c, &h, 1, 0, 3, 1, 1, 1, 0) == -1);
    CHECK(put(c, &h, 1, 0, 3, 1, 0, 0, 0) == -1);
    h.stream.Data1++;
    CHECK(put(c, &h, 1, 0, 3, 1, 0, 1, 0) == -1);
    h.stream.Data1--;
    for (first = 0; first < RECMP_ORDER_MAX; first += 45) {
        unsigned count = RECMP_ORDER_MAX - first < 45 ? RECMP_ORDER_MAX - first : 45;
        int result = put(c, &h, 3, 0, 7, RECMP_ORDER_MAX, first, count, first / 45);
        CHECK(result == (first + count == RECMP_ORDER_MAX ? 1 : 0));
        h.sequence++;
    }
    CHECK(tagpu_recmp_orders_get(c, 3, 0, &state, image, RECMP_ORDER_MAX));
    for (i = 0; i < RECMP_ORDER_MAX; i++) CHECK(image[i * RECMP_ORDER_BYTES] == i / 45);
    /* Leave less than one maximum list, then prove an overflow preserves existing lists. */
    for (i = 4; i <= 33; i++) {
        for (first = 0; first < RECMP_ORDER_MAX; first += 45) {
            unsigned count = RECMP_ORDER_MAX - first < 45 ? RECMP_ORDER_MAX - first : 45;
            int result = put(c, &h, i, 0, 7, RECMP_ORDER_MAX, first, count, i);
            CHECK(result == (first + count == RECMP_ORDER_MAX ? 1 : 0));
            h.sequence++;
        }
    }
    for (first = 0; first < RECMP_ORDER_MAX; first += 45) {
        unsigned count = RECMP_ORDER_MAX - first < 45 ? RECMP_ORDER_MAX - first : 45;
        int result = put(c, &h, 1, 0, 3, RECMP_ORDER_MAX, first, count, 0xFF);
        CHECK(result == (first + count == RECMP_ORDER_MAX ? -1 : 0));
        h.sequence++;
    }
    CHECK(tagpu_recmp_orders_get(c, 1, 0, &state, NULL, 0) && !state.count);
    CHECK(tagpu_recmp_orders_get(c, 2, 1, &state, image, 2) && image[0] == 0x33);
    tagpu_recmp_orders_drop(c, 3);
    CHECK(!tagpu_recmp_orders_get(c, 3, 0, &state, image, RECMP_ORDER_MAX));
    tagpu_recmp_orders_end(c);
    puts("PASS: transactional queues, partial source cannot seal, independent source/application clocks, interleaved AI source, missing/duplicate/stale chunks, incarnation and bounds, maximum list, fixed pool exhaustion");
    return 0;
}
