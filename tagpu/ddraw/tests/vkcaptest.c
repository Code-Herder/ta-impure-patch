/* The GPU memory cap's ledger (inc/tagpu_vkcap.h): the charge's rounding and saturation, the
   figures `gpumem=` may name, the bound held <= cap through a seeded random walk of allocations
   and frees against a plain model, a full table, a cap of none, and the door's direct road for
   every figure that is no cap. Built and run by
   tests/vkcaptest.py on the host, with the sanitizers. Exit 0 when nothing failed. */
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_vkcap.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static TAGPU_VKCAP L;

static uint64_t rng = 88172645463325252ull;
static uint64_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static void charges(void)
{
    const uint64_t G = TAGPU_VKCAP_GRANULE;
    CHECK(G == 65536);
    CHECK(tagpu_vkcap_charge(0) == G);
    CHECK(tagpu_vkcap_charge(1) == G);
    CHECK(tagpu_vkcap_charge(16) == G);
    CHECK(tagpu_vkcap_charge(G - 1) == G);
    CHECK(tagpu_vkcap_charge(G) == G);
    CHECK(tagpu_vkcap_charge(G + 1) == 2 * G);
    CHECK(tagpu_vkcap_charge((uint64_t)192 << 20) == (uint64_t)192 << 20);
    CHECK(tagpu_vkcap_charge(UINT64_MAX - G + 1) == UINT64_MAX - G + 1);
    CHECK(tagpu_vkcap_charge(UINT64_MAX - G + 2) == UINT64_MAX);
    CHECK(tagpu_vkcap_charge(UINT64_MAX) == UINT64_MAX);
}

static void figures(void)
{
    CHECK(!tagpu_vkcap_mib_ok(0));
    CHECK(!tagpu_vkcap_mib_ok(-768));
    CHECK(!tagpu_vkcap_mib_ok(63));
    CHECK(tagpu_vkcap_mib_ok(64));
    CHECK(tagpu_vkcap_mib_ok(768));
    CHECK(tagpu_vkcap_mib_ok(1L << 20));
    CHECK(!tagpu_vkcap_mib_ok((1L << 20) + 1));
    tagpu_vkcap_init(&L, 768);
    CHECK(L.cap == (uint64_t)768 << 20);
    CHECK(tagpu_vkcap_headroom(&L) == (uint64_t)768 << 20);
    tagpu_vkcap_init(&L, 63);
    CHECK(L.cap == 0);
}

/* A cap of none records nothing and admits nothing: the door never asks it then, and a ledger
   that answered yes would claim a bound it does not keep. */
static void none(void)
{
    tagpu_vkcap_init(&L, 0);
    CHECK(!tagpu_vkcap_fits(&L, TAGPU_VKCAP_GRANULE));
    CHECK(!tagpu_vkcap_add(&L, 1, TAGPU_VKCAP_GRANULE));
    CHECK(L.n == 0 && L.held == 0);
    CHECK(tagpu_vkcap_headroom(&L) == 0);
    CHECK(tagpu_vkcap_remove(&L, 1) == 0);
}

/* The door's road (tagpu_vk.c cap_arm latches it): every figure that is no cap -- none, and
   each one tagpu_vkcap_mib_ok refuses -- goes straight to the driver with no lock and no ledger,
   and every cap goes through the ledger. A direct road over a ledger that admits something would
   let an allocation past the cap; a ledger road with no cap would refuse every allocation. */
static void direct(void)
{
    static const long none[] = { 0, -768, 1, 63, (1L << 20) + 1 };
    static const long caps[] = { 64, 768, 920, 1L << 20 };
    unsigned i;
    for (i = 0; i < sizeof none / sizeof none[0]; i++) {
        tagpu_vkcap_init(&L, none[i]);
        CHECK(tagpu_vkcap_direct(&L));
        CHECK(!tagpu_vkcap_fits(&L, TAGPU_VKCAP_GRANULE));
    }
    for (i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        tagpu_vkcap_init(&L, caps[i]);
        CHECK(!tagpu_vkcap_direct(&L));
        CHECK(tagpu_vkcap_fits(&L, TAGPU_VKCAP_GRANULE));
    }
    /* a device without a cap after one with: the road is the new device's, the old ledger gone */
    tagpu_vkcap_init(&L, 768);
    CHECK(tagpu_vkcap_add(&L, 1, TAGPU_VKCAP_GRANULE));
    tagpu_vkcap_init(&L, 0);
    CHECK(tagpu_vkcap_direct(&L) && L.n == 0 && L.held == 0);
}

/* At the edge: the last granule fits, one more does not, a charge past the cap never does, and a
   saturated charge cannot wrap the test. */
static void edge(void)
{
    const uint64_t cap = (uint64_t)64 << 20, G = TAGPU_VKCAP_GRANULE;
    tagpu_vkcap_init(&L, 64);
    CHECK(tagpu_vkcap_add(&L, 1, cap - G));
    CHECK(tagpu_vkcap_fits(&L, G));
    CHECK(!tagpu_vkcap_fits(&L, 2 * G));
    CHECK(!tagpu_vkcap_fits(&L, UINT64_MAX));
    CHECK(tagpu_vkcap_add(&L, 2, G));
    CHECK(L.held == cap && tagpu_vkcap_headroom(&L) == 0);
    CHECK(!tagpu_vkcap_add(&L, 3, G));
    CHECK(L.n == 2 && L.held == cap);
    CHECK(tagpu_vkcap_remove(&L, 1) == cap - G);
    CHECK(L.held == G && L.n == 1 && L.ent[0].mem == 2);
    CHECK(tagpu_vkcap_remove(&L, 1) == 0);
    CHECK(L.peak == cap && L.npeak == 2);
    tagpu_vkcap_init(&L, 64);
    CHECK(!tagpu_vkcap_add(&L, 1, cap + G));
    CHECK(!tagpu_vkcap_add(&L, 1, UINT64_MAX));
    CHECK(L.n == 0 && L.held == 0);
}

/* A full table refuses even with room under the cap, and takes again once one entry goes. */
static void full(void)
{
    uint32_t i;
    tagpu_vkcap_init(&L, 1L << 20);
    for (i = 0; i < TAGPU_VKCAP_MAXALLOC; i++)
        CHECK(tagpu_vkcap_add(&L, 1000 + i, TAGPU_VKCAP_GRANULE));
    CHECK(L.n == TAGPU_VKCAP_MAXALLOC);
    CHECK(!tagpu_vkcap_fits(&L, TAGPU_VKCAP_GRANULE));
    CHECK(!tagpu_vkcap_add(&L, 1, TAGPU_VKCAP_GRANULE));
    CHECK(tagpu_vkcap_remove(&L, 1000 + 17) == TAGPU_VKCAP_GRANULE);
    CHECK(tagpu_vkcap_add(&L, 1, TAGPU_VKCAP_GRANULE));
    CHECK(L.held == (uint64_t)TAGPU_VKCAP_MAXALLOC * TAGPU_VKCAP_GRANULE);
}

/* The door's use, against a model: allocations of random sizes, some far past the cap, and frees
   of live, dead and never-seen handles. The ledger must refuse exactly what would take the model
   past the cap or the table, and hold held <= cap after every step. */
#define LIVE_MAX (TAGPU_VKCAP_MAXALLOC + 64)
static uint64_t liveMem[LIVE_MAX], liveCharge[LIVE_MAX];

static void walk(unsigned steps)
{
    const long mib = 768;
    const uint64_t cap = (uint64_t)mib << 20;
    uint64_t model = 0, handle = 1, peak = 0;
    unsigned nlive = 0, s, refused = 0;
    tagpu_vkcap_init(&L, mib);
    for (s = 0; s < steps; s++) {
        uint64_t r = next();
        if (r % 3 != 0 || nlive == 0) {
            uint64_t size;
            switch (next() % 4) {
            case 0:  size = 1 + next() % 4096; break;
            case 1:  size = 1 + next() % ((uint64_t)8 << 20); break;
            case 2:  size = 1 + next() % ((uint64_t)256 << 20); break;
            default: size = next() % ((uint64_t)2 << 30); break;
            }
            {
                uint64_t charge = tagpu_vkcap_charge(size);
                int want = nlive < TAGPU_VKCAP_MAXALLOC && model + charge <= cap;
                int got = tagpu_vkcap_add(&L, handle, charge);
                CHECK(got == want);
                if (got) {
                    liveMem[nlive] = handle;
                    liveCharge[nlive] = charge;
                    nlive++;
                    model += charge;
                    if (model > peak) peak = model;
                } else {
                    refused++;
                }
                handle++;
            }
        } else if (r % 7 == 0) {
            CHECK(tagpu_vkcap_remove(&L, handle + 1000000) == 0);
        } else {
            unsigned k = (unsigned)(next() % nlive);
            CHECK(tagpu_vkcap_remove(&L, liveMem[k]) == liveCharge[k]);
            model -= liveCharge[k];
            liveMem[k] = liveMem[nlive - 1];
            liveCharge[k] = liveCharge[nlive - 1];
            nlive--;
        }
        CHECK(L.held == model);
        CHECK(L.held <= L.cap);
        CHECK(L.n == nlive);
        CHECK(tagpu_vkcap_headroom(&L) == cap - model);
    }
    CHECK(L.peak == peak);
    printf("vkcaptest: %u steps, %u refused, peak %llu MiB of %ld\n", steps, refused,
           (unsigned long long)(peak >> 20), mib);
}

int main(int argc, char** argv)
{
    unsigned steps = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 200000;
    charges();
    figures();
    none();
    direct();
    edge();
    full();
    walk(steps);
    if (fails) {
        printf("vkcaptest: %d check(s) failed\n", fails);
        return 1;
    }
    printf("vkcaptest: every check holds\n");
    return 0;
}
