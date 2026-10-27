/* tagpu_cobscan.c — Create()'s hides, read from a COB block, and the pieces of a model they
   hide. The contract and the engine facts are in tagpu_cobscan.h; this file reads only its
   arguments, and every read of the block is bounded by the length it was given. The build
   ghost's mask (tagpu_datakeys.c) and the in-game wiki's models both read what it leaves, so a
   change here changes both. */
#include <string.h>
#include "tagpu_cobscan.h"

/* THE COB HEADER, as the file has it and as 0x4B2450 leaves it (offsets relocated in place).
   Nothing in the loader checks a count or an offset. The entry-point table holds code offsets
   in dwords and is not relocated; the two name tables have every entry relocated. */
#define COB_VERSION   0x00
#define COB_NSCRIPTS  0x04
#define COB_NPIECES   0x08
#define COB_CODELEN   0x0C      /* dwords                                      */
#define COB_ENTRIES   0x18      /* int32[nscripts]: code offsets, dwords       */
#define COB_SNAMES    0x1C      /* name offsets[nscripts]                      */
#define COB_PNAMES    0x20      /* name offsets[npieces]                       */
#define COB_CODE      0x24      /* int32[codelen]                              */
#define COB_HEADER    0x2C      /* the last header word the loader reads, +4   */

#define NAMECMP 256             /* the longest name a compare reads            */

/* the dword at `off`, which the caller has bounded inside the block */
static uint32_t word(const char* cob, uint32_t off)
{
    uint32_t v;
    memcpy(&v, cob + off, sizeof v);
    return v;
}

/* the bytes from the offset a stored value names to the end of the block, 0 when it is
   outside it (a value below the base wraps past `size`) */
static unsigned room_at(unsigned size, uint32_t base, uint32_t value)
{
    uint32_t o = value - base;
    return o < size ? (unsigned)(size - o) : 0u;
}

/* the name a stored value names, with its room; the block's start when the value is outside
   it, where a room of 0 lets no compare read a byte */
static const char* name_at(const char* cob, unsigned size, uint32_t base, uint32_t value,
                           unsigned* room)
{
    *room = room_at(size, base, value);
    return *room ? cob + (value - base) : cob;
}

#define OP_SHOW 0x10005000u
#define OP_HIDE 0x10006000u

/* The opcodes whose length is certain, with their inline operand counts: the prologue walk
   stops at the first opcode not here. The same table as tools/ta3do's COB_PROLOGUE, which
   measured the stock hides with it. */
static const struct { uint32_t op; unsigned char inl; } PROLOGUE[] = {
    { OP_SHOW, 1 }, { OP_HIDE, 1 },
    { 0x10007000u, 1 }, { 0x10008000u, 1 },                   /* CACHE, DONT_CACHE      */
    { 0x1000D000u, 1 }, { 0x1000E000u, 1 },                   /* SHADE, DONT_SHADE      */
    { 0x10001000u, 2 }, { 0x10002000u, 2 },                   /* MOVE, TURN             */
    { 0x1000B000u, 2 }, { 0x1000C000u, 2 },                   /* MOVE_NOW, TURN_NOW     */
    { 0x10021001u, 1 }, { 0x10021002u, 1 }, { 0x10021004u, 1 }, /* PUSH_CONSTANT/LOCAL/STATIC */
    { 0x10023002u, 1 }, { 0x10023004u, 1 },                   /* POP_LOCAL_VAR, POP_STATIC */
    { 0x10022000u, 0 }, { 0x10024000u, 0 },                   /* CREATE_LOCAL_VAR, POP_STACK */
    { 0x10031000u, 0 }, { 0x10032000u, 0 }, { 0x10033000u, 0 },
    { 0x10034000u, 0 }, { 0x10034001u, 0 },
};

static int op_inline(uint32_t op)
{
    unsigned i;
    for (i = 0; i < sizeof PROLOGUE / sizeof PROLOGUE[0]; i++)
        if (PROLOGUE[i].op == op) return PROLOGUE[i].inl;
    return -1;
}

/* _stricmp's answer (0x4F8A70, the engine's piece and script matcher: A-Z fold) for a COB name
   `a`, which has `room` bytes before its block ends, and a model node's name `b`: 1 equal,
   0 not. -1 when the two agree until `a`'s room or NAMECMP runs out without ending, or `b` is
   NULL (refused by the caller's range test): the caller then refuses the mask rather than
   guess what the engine's unbounded compare said. */
static int name_eq(const char* a, unsigned room, const char* b)
{
    unsigned i, lim = room < NAMECMP ? room : NAMECMP;
    if (!b) return -1;
    for (i = 0; i < lim; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return -1;
}

/* the same, against a lower-case literal of ours */
static int name_is(const char* a, unsigned room, const char* lit)
{
    unsigned i, lim = room < NAMECMP ? room : NAMECMP;
    for (i = 0; i < lim; i++) {
        int ca = (unsigned char)a[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (ca != (unsigned char)lit[i]) return 0;
        if (!ca) return 1;
    }
    return -1;
}

int tagpu_cobscan_hides(const char* cob, unsigned size, uint32_t base,
                        uint32_t hid[TAGPU_COB_HIDWORDS])
{
    int32_t ns, np, cl, i, ci = -1, pc;
    uint32_t entries, snames, code;
    if (size < COB_HEADER || (int32_t)word(cob, COB_VERSION) != 4) return -1;
    ns = (int32_t)word(cob, COB_NSCRIPTS);
    np = (int32_t)word(cob, COB_NPIECES);
    cl = (int32_t)word(cob, COB_CODELEN);
    if (ns <= 0 || np < 0 || np > TAGPU_COB_MAXPIECES || cl <= 0) return -1;
    if (room_at(size, base, word(cob, COB_ENTRIES)) / 4 < (unsigned)ns ||
        room_at(size, base, word(cob, COB_SNAMES))  / 4 < (unsigned)ns ||
        room_at(size, base, word(cob, COB_CODE))    / 4 < (unsigned)cl) return -1;
    entries = word(cob, COB_ENTRIES) - base;
    snames  = word(cob, COB_SNAMES)  - base;
    code    = word(cob, COB_CODE)    - base;
    for (i = 0; i < ns; i++) {
        unsigned room;
        const char* nm = name_at(cob, size, base, word(cob, snames + 4u * (uint32_t)i), &room);
        int e = name_is(nm, room, "create");
        if (e < 0) return -1;
        if (e) { ci = i; break; }
    }
    if (ci < 0) return 0;
    memset(hid, 0, (size_t)TAGPU_COB_HIDWORDS * sizeof *hid);
    pc = (int32_t)word(cob, entries + 4u * (uint32_t)ci);
    while (pc >= 0 && pc < cl) {
        uint32_t op = word(cob, code + 4u * (uint32_t)pc);
        int inl = op_inline(op);
        if (inl < 0) break;
        if ((op == OP_HIDE || op == OP_SHOW) && pc + 1 < cl) {
            int32_t k = (int32_t)word(cob, code + 4u * (uint32_t)(pc + 1));
            if (k >= 0 && k < np) {
                if (op == OP_HIDE) hid[k >> 5] |= 1u << (k & 31);
                else               hid[k >> 5] &= ~(1u << (k & 31));
            }
        }
        pc += 1 + inl;
    }
    return np;
}

const char* tagpu_cobscan_piece(const char* cob, unsigned size, uint32_t base, int k,
                                unsigned* room)
{
    int32_t np;
    uint32_t pnames;
    const char* nm;
    if (size < COB_HEADER || k < 0) return NULL;
    np = (int32_t)word(cob, COB_NPIECES);
    if (k >= np || room_at(size, base, word(cob, COB_PNAMES)) / 4 <= (unsigned)k) return NULL;
    pnames = word(cob, COB_PNAMES) - base;
    nm = name_at(cob, size, base, word(cob, pnames + 4u * (uint32_t)k), room);
    return *room ? nm : NULL;
}

int tagpu_cobscan_mask(const char* cob, unsigned size, uint32_t base,
                       const char** prim, const char** pname, int nprim,
                       const char* const* walk, int np, uint32_t* bits)
{
    uint32_t hid[TAGPU_COB_HIDWORDS];
    uint32_t pnames;
    int ncob, k, g, i, j, any = 0, upto = -1;

    if (!cob) return 0;                                /* no script: nothing hidden */
    if (!size) return -2;
    ncob = tagpu_cobscan_hides(cob, size, base, hid);
    if (ncob <= 0) return ncob;
    for (k = 0; k < ncob; k++) if (hid[k >> 5] >> (k & 31) & 1) upto = k;
    if (upto < 0) return 0;
    if (room_at(size, base, word(cob, COB_PNAMES)) / 4 < (unsigned)ncob) return -1;
    if (nprim <= 0) return -1;
    pnames = word(cob, COB_PNAMES) - base;
    for (i = 0; i < ncob && i < nprim && i <= upto; i++) {
        unsigned room;
        const char* nm = name_at(cob, size, base, word(cob, pnames + 4u * (uint32_t)i), &room);
        for (j = i; j < nprim; j++) {
            int e = name_eq(nm, room, pname[j]);
            if (e < 0) return -1;
            if (e) break;
        }
        if (j < nprim && j != i) {
            const char* t = prim[i];  prim[i]  = prim[j];  prim[j]  = t;
            t = pname[i];             pname[i] = pname[j]; pname[j] = t;
        }
    }
    for (k = 0; k <= upto && k < nprim; k++) {
        if (!(hid[k >> 5] >> (k & 31) & 1)) continue;
        any = 1;
        for (g = 0; g < np; g++)
            if (walk[g] == prim[k]) bits[g >> 5] |= 1u << (g & 31);
    }
    return any;
}
