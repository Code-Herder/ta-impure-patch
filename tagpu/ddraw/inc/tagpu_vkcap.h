#ifndef TAGPU_VKCAP_H
#define TAGPU_VKCAP_H
/* THE GPU MEMORY CAP'S LEDGER: the device-local memory one process holds, allocation by
   allocation, against a ceiling a harness launch sets (`gpumem=` in impure.cfg, written by tacli
   inside a run-lock hold; tools/runlock-vram.cfg sizes the run lock's GPU tokens on it).
   tagpu_vk.c's allocation door is its only user; this header is the arithmetic, engine-free and
   Win32-free, so tests/vkcaptest.py builds and checks it natively.

   THE BOUND, by construction: `held` is the sum of the charges of the allocations in the table,
   and an allocation is recorded only through tagpu_vkcap_add, which refuses one that would take
   `held` past `cap`. The door asks before the driver allocates and records under the same lock,
   so no allocation of the capped device escapes the sum: held <= cap at every instant.

   THE CHARGE is the size asked for, rounded up to TAGPU_VKCAP_GRANULE: a driver hands out device
   memory in pages, so a 16-byte buffer still costs one [INFERRED: the page size is the driver's;
   64 KiB is NVIDIA's small page]. What the driver holds outside vkAllocateMemory -- the
   swapchain's images, pipelines, descriptor and command pools -- is not in the ledger: the run
   lock's per-game figure adds an allowance for it (tools/runlock-vram.cfg).

   A FULL TABLE REFUSES. An allocation the ledger cannot record could not be taken off the sum at
   its free, so it is refused as a full device refuses one. TAGPU_VKCAP_MAXALLOC is set well past
   the passes' own bounds -- a few images and buffers per pass and frame slot, the UI's 32 twins
   (tagpu_vk_gui.c TW_MAX) -- [INFERRED from those bounds, not counted]; the door logs the most it
   held (`npeak`) when the device comes down. */

#include <stdint.h>

#define TAGPU_VKCAP_GRANULE  ((uint64_t)64 << 10)
#define TAGPU_VKCAP_MAXALLOC 4096
/* The caps `gpumem=` may name, in MiB: under 64 the lane's own swapchain depth and the passes'
   first atlases would be refused, so a smaller figure is a typing error, not a cap; the top is
   past any device. */
#define TAGPU_VKCAP_MIN_MIB  64
#define TAGPU_VKCAP_MAX_MIB  (1L << 20)

typedef struct {
    uint64_t mem;                       /* the VkDeviceMemory handle's bits */
    uint64_t charge;
} TAGPU_VKCAP_ENT;

typedef struct {
    uint64_t cap;                       /* bytes; 0 = no cap: nothing is recorded */
    uint64_t held;                      /* the charges of ent[0..n), <= cap */
    uint64_t peak;                      /* the most `held` has been */
    uint32_t n;
    uint32_t npeak;                     /* the most entries the table has held */
    uint32_t refused;                   /* allocations the cap or the table refused */
    TAGPU_VKCAP_ENT ent[TAGPU_VKCAP_MAXALLOC];
} TAGPU_VKCAP;

/* 1 when `mib` is a cap `gpumem=` may name. */
static inline int tagpu_vkcap_mib_ok(long mib)
{
    return mib >= TAGPU_VKCAP_MIN_MIB && mib <= TAGPU_VKCAP_MAX_MIB;
}

/* What an allocation of `size` bytes is charged: rounded up to the granule, at least one, and
   saturating rather than wrapping, so no size reads as small. */
static inline uint64_t tagpu_vkcap_charge(uint64_t size)
{
    if (size == 0) return TAGPU_VKCAP_GRANULE;
    if (size > UINT64_MAX - (TAGPU_VKCAP_GRANULE - 1)) return UINT64_MAX;
    return (size + TAGPU_VKCAP_GRANULE - 1) & ~(TAGPU_VKCAP_GRANULE - 1);
}

/* An empty ledger with a cap of `mib` MiB, or none for 0 (or for a figure tagpu_vkcap_mib_ok
   refuses: the caller has said so already). */
static inline void tagpu_vkcap_init(TAGPU_VKCAP* c, long mib)
{
    c->cap = tagpu_vkcap_mib_ok(mib) ? (uint64_t)mib << 20 : 0;
    c->held = c->peak = 0;
    c->n = c->npeak = c->refused = 0;
}

/* 1 when an allocation charged `charge` may be made now: the table has room and the sum stays at
   or under the cap. Written so that no operand can wrap: held <= cap holds on entry. */
static inline int tagpu_vkcap_fits(const TAGPU_VKCAP* c, uint64_t charge)
{
    return c->cap && c->n < TAGPU_VKCAP_MAXALLOC && charge <= c->cap - c->held;
}

/* Record `mem` charged `charge`: 1, or 0 with nothing recorded when it does not fit. */
static inline int tagpu_vkcap_add(TAGPU_VKCAP* c, uint64_t mem, uint64_t charge)
{
    if (!tagpu_vkcap_fits(c, charge)) return 0;
    c->ent[c->n].mem = mem;
    c->ent[c->n].charge = charge;
    c->n++;
    if (c->n > c->npeak) c->npeak = c->n;
    c->held += charge;
    if (c->held > c->peak) c->peak = c->held;
    return 1;
}

/* Take `mem` off the sum: its charge, or 0 for a handle the ledger does not hold (a host-memory
   allocation, which was never charged). The last entry fills the hole. */
static inline uint64_t tagpu_vkcap_remove(TAGPU_VKCAP* c, uint64_t mem)
{
    uint32_t i;
    for (i = 0; i < c->n; i++) {
        if (c->ent[i].mem != mem) continue;
        {
            uint64_t charge = c->ent[i].charge;
            c->held -= charge;
            c->ent[i] = c->ent[--c->n];
            return charge;
        }
    }
    return 0;
}

/* What may still be allocated under the cap, in bytes; 0 with no cap. */
static inline uint64_t tagpu_vkcap_headroom(const TAGPU_VKCAP* c)
{
    return c->cap ? c->cap - c->held : 0;
}

#endif
