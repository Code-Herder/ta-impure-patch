#ifndef TASIM_HASH_H
#define TASIM_HASH_H
/* tasim_hash.h -- the state hash's stream, shared by the core and the in-game wiki's front end: 64-bit FNV-1a
   (offset basis 0xCBF29CE484222325, prime 0x100000001B3), an integer fed as 4 bytes little-endian (two's
   complement), a double as its 8 IEEE-754 bytes little-endian, a string as its byte length (an integer) and then
   its bytes, "" for none.

   ONE STREAM, ITEM BY ITEM. tagpu_wikisim_hash interleaves the core's items with the front end's own (the input,
   the sidebar, the demo's clock) on one running state, in tagpu_wikisim.h's order; each core item is fed by its
   feeder below, which feeds exactly the bytes of that item, so the front end's hash is the same value whoever
   feeds an item. tasim_hash is the core's alone: the tick and the core's items, in the same order, from the
   offset basis. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "tasim_state.h"

#define TASIM_FNV_BASIS 0xCBF29CE484222325ull

static inline void fnv_bytes(uint64_t* h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) {
        *h ^= *b++;
        *h *= 0x100000001B3ull;
    }
}

static inline void fnv_int(uint64_t* h, int v)
{
    const uint32_t u = (uint32_t)v;
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++) b[i] = (unsigned char)(u >> (8 * i));
    fnv_bytes(h, b, 4);
}

static inline void fnv_double(uint64_t* h, double v)
{
    uint64_t u;
    unsigned char b[8];
    int i;
    memcpy(&u, &v, sizeof u);
    for (i = 0; i < 8; i++) b[i] = (unsigned char)(u >> (8 * i));
    fnv_bytes(h, b, 8);
}

static inline void fnv_str(uint64_t* h, const char* str)
{
    const size_t n = str ? strlen(str) : 0;
    fnv_int(h, (int)n);
    fnv_bytes(h, str, n);
}

/* the core's items, each as tagpu_wikisim.h's item list names it */
void     tasim_hash_map(uint64_t* h, const TASIM* s);                             /* item 2                    */
void     tasim_hash_units(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs);   /* item 3                    */
void     tasim_hash_cob(uint64_t* h, const TASIM* s);                             /* item 8's COB host         */
void     tasim_hash_fx(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs);      /* items 9 and 10            */
void     tasim_hash_feats(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs);   /* item 11's features        */
void     tasim_hash_econ(uint64_t* h, const TASIM* s);                            /* item 12, a game's only    */
void     tasim_hash_deaths(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs);  /* item 14, a game's only    */
/* the core's own hash: the tick (an integer), then the feeders above in their order, from the offset basis */
uint64_t tasim_hash(const TASIM* s, const TASIM_DEFS* defs);

#endif
